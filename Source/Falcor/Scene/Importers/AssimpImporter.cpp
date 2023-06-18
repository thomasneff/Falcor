/***************************************************************************
 # Copyright (c) 2015-22, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "AssimpImporter.h"
#include "Core/Assert.h"
#include "Core/API/Device.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"
#include "Utils/NumericRange.h"
#include "Utils/Timing/TimeReport.h"
#include "Utils/Math/Common.h"
#include "Utils/Math/FalcorMath.h"
#include "Scene/Importer.h"
#include "Scene/SceneBuilder.h"
#include "Scene/Material/Material.h"
#include "Scene/Material/StandardMaterial.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/pbrmaterial.h>

#include <execution>
#include <fstream>

namespace Falcor
{
    namespace
    {
        // Global camera animation interpolation and warping configuration.
        // Assimp does not provide enough information to determine this from data.
        static const Animation::InterpolationMode kCameraInterpolationMode = Animation::InterpolationMode::Linear;
        static const bool kCameraEnableWarping = true;

        using BoneMeshMap = std::map<std::string, std::vector<uint32_t>>;
        using MeshInstanceList = std::vector<std::vector<const aiNode*>>;

        /** Converts specular power to roughness. Note there is no "the conversion".
            Reference: http://simonstechblog.blogspot.com/2011/12/microfacet-brdf.html
            \param specPower specular power of an obsolete Phong BSDF
        */
        float convertSpecPowerToRoughness(float specPower)
        {
            return clamp(sqrt(2.0f / (specPower + 2.0f)), 0.f, 1.f);
        }

        enum class ImportMode {
            Default,
            OBJ,
            GLTF2,
        };

        rmcv::mat4 aiCast(const aiMatrix4x4& aiMat)
        {
            rmcv::mat4 m{
            aiMat.a1, aiMat.a2, aiMat.a3, aiMat.a4,
            aiMat.b1, aiMat.b2, aiMat.b3, aiMat.b4,
            aiMat.c1, aiMat.c2, aiMat.c3, aiMat.c4,
            aiMat.d1, aiMat.d2, aiMat.d3, aiMat.d4
            };

            return m;
        }

        float3 aiCast(const aiColor3D& ai)
        {
            return float3(ai.r, ai.g, ai.b);
        }

        float3 aiCast(const aiVector3D& val)
        {
            return float3(val.x, val.y, val.z);
        }

        glm::quat aiCast(const aiQuaternion& q)
        {
            return glm::quat(q.w, q.x, q.y, q.z);
        }

        /** Mapping from ASSIMP to Falcor texture type.
        */
        struct TextureMapping
        {
            aiTextureType aiType;
            unsigned int aiIndex;
            Material::TextureSlot targetType;
        };

        /** Mapping tables for different import modes.
        */
        static const std::vector<TextureMapping> kTextureMappings[3] =
        {
            // Default mappings
            {
                { aiTextureType_DIFFUSE, 0, Material::TextureSlot::BaseColor },
                { aiTextureType_SPECULAR, 0, Material::TextureSlot::Specular },
                { aiTextureType_EMISSIVE, 0, Material::TextureSlot::Emissive },
                { aiTextureType_NORMALS, 0, Material::TextureSlot::Normal },
            },
            // OBJ mappings
            {
                { aiTextureType_DIFFUSE, 0, Material::TextureSlot::BaseColor },
                { aiTextureType_SPECULAR, 0, Material::TextureSlot::Specular },
                { aiTextureType_EMISSIVE, 0, Material::TextureSlot::Emissive },
                // OBJ does not offer a normal map, thus we use the bump map instead.
                { aiTextureType_HEIGHT, 0, Material::TextureSlot::Normal },
                { aiTextureType_DISPLACEMENT, 0, Material::TextureSlot::Normal },
            },
            // GLTF2 mappings
            {
                { aiTextureType_DIFFUSE, 0, Material::TextureSlot::BaseColor },
                { aiTextureType_EMISSIVE, 0, Material::TextureSlot::Emissive },
                { aiTextureType_NORMALS, 0, Material::TextureSlot::Normal },
                // GLTF2 exposes metallic roughness texture.
                { AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE, Material::TextureSlot::Specular },
            }
        };

        class ImporterData
        {
        public:
            ImporterData(const std::filesystem::path& path, const aiScene* pAiScene, SceneBuilder& sceneBuilder, const SceneBuilder::InstanceMatrices& modelInstances_)
                : path(path)
                , pScene(pAiScene)
                , modelInstances(modelInstances_)
                , builder(sceneBuilder)
            {}
            std::filesystem::path path;
            const aiScene* pScene;
            SceneBuilder& builder;
            std::map<uint32_t, Material::SharedPtr> materialMap;
            std::map<uint32_t, MeshID> meshMap; // Assimp mesh index to Falcor mesh ID
            const SceneBuilder::InstanceMatrices& modelInstances;
            std::map<std::string, rmcv::mat4> localToBindPoseMatrices;

            NodeID getFalcorNodeID(const aiNode* pNode) const
            {
                return mAiToFalcorNodeID.at(pNode);
            }

            NodeID getFalcorNodeID(const std::string& aiNodeName, uint32_t index) const
            {
                try
                {
                    return getFalcorNodeID(mAiNodes.at(aiNodeName)[index]);
                }
                catch (const std::exception&)
                {
                    return NodeID::Invalid();
                }
            }

            uint32_t getNodeInstanceCount(const std::string& nodeName) const
            {
                return (uint32_t)mAiNodes.at(nodeName).size();
            }

            void addAiNode(const aiNode* pNode, NodeID falcorNodeID)
            {
                FALCOR_ASSERT(mAiToFalcorNodeID.find(pNode) == mAiToFalcorNodeID.end());
                mAiToFalcorNodeID[pNode] = falcorNodeID;

                if (mAiNodes.find(pNode->mName.C_Str()) == mAiNodes.end())
                {
                    mAiNodes[pNode->mName.C_Str()] = {};
                }
                mAiNodes[pNode->mName.C_Str()].push_back(pNode);
            }

        private:
            std::map<const aiNode*, NodeID> mAiToFalcorNodeID;
            std::map<const std::string, std::vector<const aiNode*>> mAiNodes;

        };

        using KeyframeList = std::list<Animation::Keyframe>;

        struct AnimationChannelData
        {
            uint32_t posIndex = 0;
            uint32_t rotIndex = 0;
            uint32_t scaleIndex = 0;
        };

        template<typename AiType, typename FalcorType>
        bool parseAnimationChannel(const AiType* pKeys, uint32_t count, double time, uint32_t& currentIndex, FalcorType& falcor)
        {
            if (currentIndex >= count) return true;

            if (pKeys[currentIndex].mTime == time)
            {
                falcor = aiCast(pKeys[currentIndex].mValue);
                currentIndex++;
            }

            return currentIndex >= count;
        }

        void resetNegativeKeyframeTimes(aiNodeAnim* pAiNode)
        {
            auto resetTime = [](auto keys, uint32_t count)
            {
                if (count > 1) FALCOR_ASSERT(keys[1].mTime >= 0);
                if (keys[0].mTime < 0) keys[0].mTime = 0;
            };
            resetTime(pAiNode->mPositionKeys, pAiNode->mNumPositionKeys);
            resetTime(pAiNode->mRotationKeys, pAiNode->mNumRotationKeys);
            resetTime(pAiNode->mScalingKeys, pAiNode->mNumScalingKeys);
        }

        void createAnimation(ImporterData& data, const aiAnimation* pAiAnim, ImportMode importMode)
        {
            FALCOR_ASSERT(pAiAnim->mNumMeshChannels == 0);
            double duration = pAiAnim->mDuration;
            double ticksPerSecond = pAiAnim->mTicksPerSecond ? pAiAnim->mTicksPerSecond : 25;
            // The GLTF2 importer in Assimp has a bug where duration and keyframe times are loaded as milliseconds instead of ticks.
            // We can fix this by using a fixed ticksPerSecond value of 1000.
            if (importMode == ImportMode::GLTF2) ticksPerSecond = 1000.0;
            double durationInSeconds = duration / ticksPerSecond;

            for (uint32_t i = 0; i < pAiAnim->mNumChannels; i++)
            {
                aiNodeAnim* pAiNode = pAiAnim->mChannels[i];
                resetNegativeKeyframeTimes(pAiNode);

                std::vector<Animation::SharedPtr> animations;
                for (uint32_t i = 0; i < data.getNodeInstanceCount(pAiNode->mNodeName.C_Str()); i++)
                {
                    Animation::SharedPtr pAnimation = Animation::create(
                        std::string(pAiNode->mNodeName.C_Str()) + "." + std::to_string(i),
                        data.getFalcorNodeID(pAiNode->mNodeName.C_Str(), i),
                        durationInSeconds
                    );
                    animations.push_back(pAnimation);
                    data.builder.addAnimation(pAnimation);
                }

                uint32_t pos = 0, rot = 0, scale = 0;
                Animation::Keyframe keyframe;
                bool done = false;

                auto nextKeyTime = [&]()
                {
                    double time = -std::numeric_limits<double>::max();
                    if (pos < pAiNode->mNumPositionKeys) time = std::max(time, pAiNode->mPositionKeys[pos].mTime);
                    if (rot < pAiNode->mNumRotationKeys) time = std::max(time, pAiNode->mRotationKeys[rot].mTime);
                    if (scale < pAiNode->mNumScalingKeys) time = std::max(time, pAiNode->mScalingKeys[scale].mTime);
                    FALCOR_ASSERT(time != -std::numeric_limits<double>::max());
                    return time;
                };

                while (!done)
                {
                    double time = nextKeyTime();
                    FALCOR_ASSERT(time == 0 || (time / ticksPerSecond) > keyframe.time);
                    keyframe.time = time / ticksPerSecond;

                    // Note the order of the logical-and, we don't want to short-circuit the function calls
                    done = parseAnimationChannel(pAiNode->mPositionKeys, pAiNode->mNumPositionKeys, time, pos, keyframe.translation);
                    done = parseAnimationChannel(pAiNode->mRotationKeys, pAiNode->mNumRotationKeys, time, rot, keyframe.rotation) && done;
                    done = parseAnimationChannel(pAiNode->mScalingKeys, pAiNode->mNumScalingKeys, time, scale, keyframe.scaling) && done;
                    for(auto pAnimation : animations) pAnimation->addKeyframe(keyframe);
                }
            }
        }

        void createCameras(ImporterData& data, ImportMode importMode)
        {
            for (uint i = 0; i < data.pScene->mNumCameras; i++)
            {
                const aiCamera* pAiCamera = data.pScene->mCameras[i];
                Camera::SharedPtr pCamera = Camera::create();
                pCamera->setName(pAiCamera->mName.C_Str());
                pCamera->setPosition(aiCast(pAiCamera->mPosition));
                pCamera->setUpVector(aiCast(pAiCamera->mUp));
                pCamera->setTarget(aiCast(pAiCamera->mLookAt) + aiCast(pAiCamera->mPosition));

                // Some importers don't provide the aspect ratio, use default for that case.
                float aspectRatio = pAiCamera->mAspect != 0.f ? pAiCamera->mAspect : pCamera->getAspectRatio();
                // Load focal length only when using GLTF2, use fixed 35mm for backwards compatibility with FBX files.
                float focalLength = importMode == ImportMode::GLTF2 ? fovYToFocalLength(pAiCamera->mHorizontalFOV / aspectRatio, pCamera->getFrameHeight()) : 35.f;
                pCamera->setFocalLength(focalLength);
                pCamera->setAspectRatio(aspectRatio);
                pCamera->setDepthRange(pAiCamera->mClipPlaneNear, pAiCamera->mClipPlaneFar);

                NodeID nodeID = data.getFalcorNodeID(pAiCamera->mName.C_Str(), 0);

                if (nodeID != NodeID::Invalid())
                {
                    SceneBuilder::Node n;
                    n.name = "Camera.BaseMatrix";
                    n.parent = nodeID;
                    n.transform = pCamera->getViewMatrix();
                    // GLTF2 already uses -Z view direction convention in Assimp, FBX does not
                    if (importMode != ImportMode::GLTF2) n.transform.setCol(2,-n.transform.getCol(2));
                    nodeID = data.builder.addNode(n);
                    pCamera->setNodeID(nodeID);
                    if (data.builder.isNodeAnimated(nodeID))
                    {
                        pCamera->setHasAnimation(true);
                        data.builder.setNodeInterpolationMode(nodeID, kCameraInterpolationMode, kCameraEnableWarping);
                    }
                }

                data.builder.addCamera(pCamera);
            }
        }

        void addLightCommon(const Light::SharedPtr& pLight, const rmcv::mat4& baseMatrix, ImporterData& data, const aiLight* pAiLight)
        {
            FALCOR_ASSERT(pAiLight->mColorDiffuse == pAiLight->mColorSpecular);
            pLight->setIntensity(aiCast(pAiLight->mColorSpecular));

            // Find if the light is affected by a node
            NodeID nodeID = data.getFalcorNodeID(pAiLight->mName.C_Str(), 0);
            if (nodeID != NodeID::Invalid())
            {
                SceneBuilder::Node n;
                n.name = pLight->getName() + ".BaseMatrix";
                n.parent = nodeID;
                n.transform = baseMatrix;
                nodeID = data.builder.addNode(n);
                pLight->setHasAnimation(true);
                pLight->setNodeID(nodeID);
            }
            data.builder.addLight(pLight);
        }

        void createDirLight(ImporterData& data, const aiLight* pAiLight)
        {
            DirectionalLight::SharedPtr pLight = DirectionalLight::create(pAiLight->mName.C_Str());
            float3 direction = normalize(aiCast(pAiLight->mDirection));
            pLight->setWorldDirection(direction);
            rmcv::mat4 base;
            base.setCol(2, float4(-direction, 0));
            addLightCommon(pLight, base, data, pAiLight);
        }

        void createPointLight(ImporterData& data, const aiLight* pAiLight)
        {
            PointLight::SharedPtr pLight = PointLight::create(pAiLight->mName.C_Str());
            float3 position = aiCast(pAiLight->mPosition);
            float3 direction = aiCast(pAiLight->mDirection);
            float3 up = aiCast(pAiLight->mUp);

            // GLTF2 may report zero vectors for direction/up in which case we need to initialize to sensible defaults.
            direction = length(direction) == 0.f ? float3(0.f, 0.f, -1.f) : normalize(direction);
            up = length(up) == 0.f ? float3(0.f, 1.f, 0.f) : normalize(up);

            pLight->setWorldPosition(position);
            pLight->setWorldDirection(direction);
            pLight->setOpeningAngle(pAiLight->mAngleOuterCone);
            pLight->setPenumbraAngle(pAiLight->mAngleOuterCone - pAiLight->mAngleInnerCone);

            float3 right = cross(direction, up);
            rmcv::mat4 base;
            // Set it as rows here, really?
            base.setCol(0, float4(right, 0));
            base.setCol(1, float4(up, 0));
            base.setCol(2, float4(-direction, 0));
            base.setCol(3, float4(position, 1));

            addLightCommon(pLight, base, data, pAiLight);
        }

        void createLights(ImporterData& data)
        {
            for (uint32_t i = 0; i < data.pScene->mNumLights; i++)
            {
                const aiLight* pAiLight = data.pScene->mLights[i];
                switch (pAiLight->mType)
                {
                case aiLightSource_DIRECTIONAL:
                    createDirLight(data, pAiLight);
                    break;
                case aiLightSource_POINT:
                case aiLightSource_SPOT:
                    createPointLight(data, pAiLight);
                    break;
                default:
                    logWarning("AssimpImporter: Light '{}' has unsupported type {}, ignoring.", pAiLight->mName.C_Str(), pAiLight->mType);
                    continue;
                }
            }
        }

        void createAnimations(ImporterData& data, ImportMode importMode)
        {
            for (uint32_t i = 0; i < data.pScene->mNumAnimations; i++)
            {
                createAnimation(data, data.pScene->mAnimations[i], importMode);
            }
        }

        void createTexCrdList(const aiVector3D* pAiTexCrd, uint32_t count, std::vector<float2>& texCrds)
        {
            texCrds.resize(count);
            for (uint32_t i = 0; i < count; i++)
            {
                FALCOR_ASSERT(pAiTexCrd[i].z == 0);
                texCrds[i] = float2(pAiTexCrd[i].x, pAiTexCrd[i].y);
            }
        }

        void createTangentList(const aiVector3D* pAiTangent, const aiVector3D* pAiBitangent, const aiVector3D* pAiNormal, uint32_t count, std::vector<float4>& tangents)
        {
            tangents.resize(count);
            for (uint32_t i = 0; i < count; i++)
            {
                // We compute the bitangent at runtime as defined by MikkTSpace: cross(N, tangent.xyz) * tangent.w.
                // Compute the orientation of the loaded bitangent here to set the sign (w) correctly.
                float3 T = float3(pAiTangent[i].x, pAiTangent[i].y, pAiTangent[i].z);
                float3 B = float3(pAiBitangent[i].x, pAiBitangent[i].y, pAiBitangent[i].z);
                float3 N = float3(pAiNormal[i].x, pAiNormal[i].y, pAiNormal[i].z);
                float sign = dot(cross(N, T), B) >= 0.f ? 1.f : -1.f;
                tangents[i] = float4(glm::normalize(T), sign);
            }
        }

        void createIndexList(const aiMesh* pAiMesh, std::vector<uint32_t>& indices)
        {
            const uint32_t perFaceIndexCount = pAiMesh->mFaces[0].mNumIndices;
            const uint32_t indexCount = pAiMesh->mNumFaces * perFaceIndexCount;

            indices.resize(indexCount);
            for (uint32_t i = 0; i < pAiMesh->mNumFaces; i++)
            {
                FALCOR_ASSERT(pAiMesh->mFaces[i].mNumIndices == perFaceIndexCount); // Mesh contains mixed primitive types, can be solved using aiProcess_SortByPType
                for (uint32_t j = 0; j < perFaceIndexCount; j++)
                {
                    indices[i * perFaceIndexCount + j] = (uint32_t)(pAiMesh->mFaces[i].mIndices[j]);
                }
            }
        }

        void loadBones(const aiMesh* pAiMesh, const ImporterData& data, std::vector<float4>& weights, std::vector<uint4>& ids)
        {
            const uint32_t vertexCount = pAiMesh->mNumVertices;

            weights.resize(vertexCount);
            ids.resize(vertexCount);

            std::fill(weights.begin(), weights.end(), float4(0.f));
            std::fill(ids.begin(), ids.end(), uint4(NodeID::kInvalidID));
            static_assert(sizeof(uint4) == 4 * sizeof(NodeID::IntType));

            for (uint32_t bone = 0; bone < pAiMesh->mNumBones; bone++)
            {
                const aiBone* pAiBone = pAiMesh->mBones[bone];
                FALCOR_ASSERT(data.getNodeInstanceCount(pAiBone->mName.C_Str()) == 1);
                NodeID aiBoneID = data.getFalcorNodeID(pAiBone->mName.C_Str(), 0);

                // The way Assimp works, the weights holds the IDs of the vertices it affects.
                // We loop over all the weights, initializing the vertices data along the way
                for (uint32_t weightID = 0; weightID < pAiBone->mNumWeights; weightID++)
                {
                    // Get the vertex the current weight affects
                    const aiVertexWeight& aiWeight = pAiBone->mWeights[weightID];

                    // Skip zero weights
                    if (aiWeight.mWeight == 0.f) continue;

                    // Get the address of the Bone ID and weight for the current vertex
                    uint4& vertexIds = ids[aiWeight.mVertexId];
                    float4& vertexWeights = weights[aiWeight.mVertexId];

                    // Find the next unused slot in the bone array of the vertex, and initialize it with the current value
                    bool emptySlotFound = false;
                    for (uint32_t j = 0; j < Scene::kMaxBonesPerVertex; j++)
                    {
                        if (vertexIds[j] == NodeID::kInvalidID)
                        {
                            vertexIds[j] = aiBoneID.getSlang();
                            vertexWeights[j] = aiWeight.mWeight;
                            emptySlotFound = true;
                            break;
                        }
                    }

                    if (!emptySlotFound)
                    {
                        logWarning("AssimpImporter: One of the vertices has too many bones attached to it. This bone will be ignored and the animation might not look correct.");
                    }
                }
            }

            // Now we need to normalize the weights for each vertex, since in some models the sum is larger than 1
            for (uint32_t i = 0; i < vertexCount; i++)
            {
                float4& w = weights[i];
                float f = 0;
                for (uint32_t j = 0; j < Scene::kMaxBonesPerVertex; j++) f += w[j];
                w /= f;
            }
        }

        void createMeshes(ImporterData& data)
        {
            const aiScene* pScene = data.pScene;
            const bool loadTangents = is_set(data.builder.getFlags(), SceneBuilder::Flags::UseOriginalTangentSpace);

            std::vector<const aiMesh*> meshes;
            for (uint32_t i = 0; i < pScene->mNumMeshes; ++i) {
                const aiMesh* pMesh = pScene->mMeshes[i];
                if (!pMesh->HasFaces())
                {
                    logWarning("AssimpImporter: Mesh '{}' has no faces, ignoring.", pMesh->mName.C_Str());
                    continue;
                }
                if (pMesh->mFaces->mNumIndices != 3)
                {
                    logWarning("AssimpImporter: Mesh '{}' is not a triangle mesh, ignoring.", pMesh->mName.C_Str());
                    continue;
                }
                meshes.push_back(pMesh);
            }

            // Pre-process meshes.
            std::vector<SceneBuilder::ProcessedMesh> processedMeshes(meshes.size());
            auto range = NumericRange<size_t>(0, meshes.size());
            std::for_each(std::execution::par, range.begin(), range.end(), [&] (size_t i) {
                const aiMesh* pAiMesh = meshes[i];
                const uint32_t perFaceIndexCount = pAiMesh->mFaces[0].mNumIndices;

                SceneBuilder::Mesh mesh;
                mesh.name = pAiMesh->mName.C_Str();
                mesh.faceCount = pAiMesh->mNumFaces;

                // Temporary memory for the vertex and index data.
                std::vector<uint32_t> indexList;
                std::vector<float2> texCrds;
                std::vector<float4> tangents;
                std::vector<uint4> boneIds;
                std::vector<float4> boneWeights;

                // Indices
                createIndexList(pAiMesh, indexList);
                FALCOR_ASSERT(indexList.size() <= std::numeric_limits<uint32_t>::max());
                mesh.indexCount = (uint32_t)indexList.size();
                mesh.pIndices = indexList.data();
                mesh.topology = Vao::Topology::TriangleList;

                // Vertices
                FALCOR_ASSERT(pAiMesh->mVertices);
                mesh.vertexCount = pAiMesh->mNumVertices;
                static_assert(sizeof(pAiMesh->mVertices[0]) == sizeof(mesh.positions.pData[0]));
                static_assert(sizeof(pAiMesh->mNormals[0]) == sizeof(mesh.normals.pData[0]));
                mesh.positions.pData = reinterpret_cast<float3*>(pAiMesh->mVertices);
                mesh.positions.frequency = SceneBuilder::Mesh::AttributeFrequency::Vertex;
                mesh.normals.pData = reinterpret_cast<float3*>(pAiMesh->mNormals);
                mesh.normals.frequency = SceneBuilder::Mesh::AttributeFrequency::Vertex;

                if (pAiMesh->HasTextureCoords(0))
                {
                    createTexCrdList(pAiMesh->mTextureCoords[0], pAiMesh->mNumVertices, texCrds);
                    FALCOR_ASSERT(!texCrds.empty());
                    mesh.texCrds.pData = texCrds.data();
                    mesh.texCrds.frequency = SceneBuilder::Mesh::AttributeFrequency::Vertex;
                }

                if (loadTangents && pAiMesh->HasTangentsAndBitangents())
                {
                    createTangentList(pAiMesh->mTangents, pAiMesh->mBitangents, pAiMesh->mNormals, pAiMesh->mNumVertices, tangents);
                    FALCOR_ASSERT(!tangents.empty());
                    mesh.tangents.pData = tangents.data();
                    mesh.tangents.frequency = SceneBuilder::Mesh::AttributeFrequency::Vertex;
                }

                if (pAiMesh->HasBones())
                {
                    loadBones(pAiMesh, data, boneWeights, boneIds);
                    mesh.boneIDs.pData = boneIds.data();
                    mesh.boneIDs.frequency = SceneBuilder::Mesh::AttributeFrequency::Vertex;
                    mesh.boneWeights.pData = boneWeights.data();
                    mesh.boneWeights.frequency = SceneBuilder::Mesh::AttributeFrequency::Vertex;
                }

                mesh.pMaterial = data.materialMap.at(pAiMesh->mMaterialIndex);

                processedMeshes[i] = data.builder.processMesh(mesh);
            });

            // Add meshes to the scene.
            // We retain a deterministic order of the meshes in the global scene buffer by adding
            // them sequentially after being processed in parallel.
            uint32_t i = 0;
            for (const auto& mesh : processedMeshes)
            {
                MeshID meshID = data.builder.addProcessedMesh(mesh);
                data.meshMap[i++] = meshID;
            }
        }

        bool isBone(ImporterData& data, const std::string& name)
        {
            return data.localToBindPoseMatrices.find(name) != data.localToBindPoseMatrices.end();
        }

        std::string getNodeType(ImporterData& data, const aiNode* pNode)
        {
            if (pNode->mNumMeshes > 0) return "mesh instance";
            if (isBone(data, pNode->mName.C_Str())) return "bone";
            else return "local transform";
        }

        void dumpSceneGraphHierarchy(ImporterData& data, const std::filesystem::path& path, aiNode* pRoot)
        {
            std::ofstream dotfile;
            dotfile.open(path);

            std::function<void(const aiNode* pNode)> dumpNode = [&dotfile, &dumpNode, &data](const aiNode* pNode)
            {
                for (uint32_t i = 0; i < pNode->mNumChildren; i++)
                {
                    const aiNode* pChild = pNode->mChildren[i];
                    std::string parent = pNode->mName.C_Str();
                    std::string parentType = getNodeType(data, pNode);
                    std::string parentID = to_string(data.getFalcorNodeID(pNode));
                    std::string me = pChild->mName.C_Str();
                    std::string myType = getNodeType(data, pChild);
                    std::string myID = to_string(data.getFalcorNodeID(pChild));
                    std::replace(parent.begin(), parent.end(), '.', '_');
                    std::replace(me.begin(), me.end(), '.', '_');
                    std::replace(parent.begin(), parent.end(), '$', '_');
                    std::replace(me.begin(), me.end(), '$', '_');

                    dotfile << parentID << " " << parent << " (" << parentType << ") " << " -> " << myID << " " << me << " (" << myType << ") " << std::endl;

                    dumpNode(pChild);
                }
            };

            // Header
            dotfile << "digraph SceneGraph {" << std::endl;
            dumpNode(pRoot);
            // Close the file
            dotfile << "}" << std::endl; // closing graph scope
            dotfile.close();
        }

        rmcv::mat4 getLocalToBindPoseMatrix(ImporterData& data, const std::string& name)
        {
            return isBone(data, name) ? data.localToBindPoseMatrices[name] : rmcv::identity<rmcv::mat4>();
        }

        void parseNode(ImporterData& data, const aiNode* pCurrent, bool hasBoneAncestor)
        {
            SceneBuilder::Node n;
            n.name = pCurrent->mName.C_Str();
            bool currentIsBone = isBone(data, n.name);
            FALCOR_ASSERT(currentIsBone == false || pCurrent->mNumMeshes == 0);

            n.parent = pCurrent->mParent ? data.getFalcorNodeID(pCurrent->mParent) : NodeID::Invalid();
            n.transform = aiCast(pCurrent->mTransformation);
            n.localToBindPose = getLocalToBindPoseMatrix(data, n.name);

            data.addAiNode(pCurrent, data.builder.addNode(n));

            // visit the children
            for (uint32_t i = 0; i < pCurrent->mNumChildren; i++)
            {
                parseNode(data, pCurrent->mChildren[i], currentIsBone || hasBoneAncestor);
            }
        }

        void createBoneList(ImporterData& data)
        {
            const aiScene* pScene = data.pScene;
            auto& boneMatrices = data.localToBindPoseMatrices;

            for (uint32_t meshID = 0; meshID < pScene->mNumMeshes; meshID++)
            {
                const aiMesh* pMesh = pScene->mMeshes[meshID];
                if (pMesh->HasBones() == false) continue;;
                for (uint32_t boneID = 0; boneID < pMesh->mNumBones; boneID++)
                {
                    boneMatrices[pMesh->mBones[boneID]->mName.C_Str()] = aiCast(pMesh->mBones[boneID]->mOffsetMatrix);
                }
            }
        }

        void createSceneGraph(ImporterData& data)
        {
            createBoneList(data);
            aiNode* pRoot = data.pScene->mRootNode;
            FALCOR_ASSERT(isBone(data, pRoot->mName.C_Str()) == false);
            parseNode(data, pRoot, false);
            // dumpSceneGraphHierarchy(data, "graph.dotfile", pRoot); // used for debugging
        }

        void addMeshInstances(ImporterData& data, aiNode* pNode)
        {
            NodeID nodeID = data.getFalcorNodeID(pNode);
            for (uint32_t mesh = 0; mesh < pNode->mNumMeshes; mesh++)
            {
                MeshID meshID = data.meshMap.at(pNode->mMeshes[mesh]);

                if (data.modelInstances.size())
                {
                    for(size_t instance = 0; instance < data.modelInstances.size(); instance++)
                    {
                        NodeID instanceNodeID = nodeID;
                        if(data.modelInstances[instance] != rmcv::mat4())
                        {
                            // Add nodes
                            SceneBuilder::Node n;
                            n.name = "Node" + to_string(nodeID) + ".instance" + std::to_string(instance);
                            n.parent = nodeID;
                            n.transform = data.modelInstances[instance];
                            instanceNodeID = data.builder.addNode(n);
                        }
                        data.builder.addMeshInstance(instanceNodeID, meshID);
                    }
                }
                else data.builder.addMeshInstance(nodeID, meshID);
            }

            // Visit the children
            for (uint32_t i = 0; i < pNode->mNumChildren; i++)
            {
                addMeshInstances(data, pNode->mChildren[i]);
            }
        }

        void loadTextures(ImporterData& data, const aiMaterial* pAiMaterial, const std::filesystem::path& searchPath, const Material::SharedPtr& pMaterial, ImportMode importMode)
        {
            const auto& textureMappings = kTextureMappings[int(importMode)];

            for (const auto& source : textureMappings)
            {
                // Skip if texture of requested type is not available
                if (pAiMaterial->GetTextureCount(source.aiType) < source.aiIndex + 1) continue;

                // Get the texture name
                aiString aiPath;
                pAiMaterial->GetTexture(source.aiType, source.aiIndex, &aiPath);
                std::string path(aiPath.data);
                // Assets may contain windows native paths, replace '\' with '/' to make compatible on Linux.
                std::replace(path.begin(), path.end(), '\\', '/');
                if (path.empty())
                {
                    logWarning("AssimpImporter: Texture has empty file name, ignoring.");
                    continue;
                }

                // Load the texture
                auto fullPath = searchPath / path;
                data.builder.loadMaterialTexture(pMaterial, source.targetType, fullPath);
            }
        }

        Material::SharedPtr createMaterial(ImporterData& data, const aiMaterial* pAiMaterial, const std::filesystem::path& searchPath, ImportMode importMode)
        {
            aiString name;
            pAiMaterial->Get(AI_MATKEY_NAME, name);

            // Parse the name
            std::string nameStr = std::string(name.C_Str());
            if (nameStr.empty())
            {
                logWarning("AssimpImporter: Material with no name found -> renaming to 'unnamed'.");
                nameStr = "unnamed";
            }

            // Determine shading model.
            // MetalRough is the default for everything except OBJ. Check that both flags aren't set simultaneously.
            ShadingModel shadingModel = ShadingModel::MetalRough;
            SceneBuilder::Flags builderFlags = data.builder.getFlags();
            FALCOR_ASSERT(!(is_set(builderFlags, SceneBuilder::Flags::UseSpecGlossMaterials) && is_set(builderFlags, SceneBuilder::Flags::UseMetalRoughMaterials)));
            if (is_set(builderFlags, SceneBuilder::Flags::UseSpecGlossMaterials) || (importMode == ImportMode::OBJ && !is_set(builderFlags, SceneBuilder::Flags::UseMetalRoughMaterials)))
            {
                shadingModel = ShadingModel::SpecGloss;
            }

            // Create an instance of the standard material. All materials are assumed to be of this type.
            StandardMaterial::SharedPtr pMaterial = StandardMaterial::create(nameStr, shadingModel);

            // Load textures. Note that loading is affected by the current shading model.
            loadTextures(data, pAiMaterial, searchPath, pMaterial, importMode);

            // Opacity
            float opacity = 1.f;
            if (pAiMaterial->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
            {
                float4 diffuse = pMaterial->getBaseColor();
                diffuse.a = opacity;
                pMaterial->setBaseColor(diffuse);
            }

            // Bump scaling
            float bumpScaling;
            if (pAiMaterial->Get(AI_MATKEY_BUMPSCALING, bumpScaling) == AI_SUCCESS)
            {
                // TODO this should probably be a multiplier to the normal map
            }

            // Shininess
            float shininess;
            if (pAiMaterial->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS)
            {
                // Convert OBJ/MTL Phong exponent to glossiness.
                if (importMode == ImportMode::OBJ)
                {
                    float roughness = convertSpecPowerToRoughness(shininess);
                    shininess = 1.f - roughness;
                }
                float4 spec = pMaterial->getSpecularParams();
                spec.a = shininess;
                pMaterial->setSpecularParams(spec);
            }

            // Refraction
            float refraction;
            if (pAiMaterial->Get(AI_MATKEY_REFRACTI, refraction) == AI_SUCCESS) pMaterial->setIndexOfRefraction(refraction);

            // Diffuse color
            aiColor3D color;
            if (pAiMaterial->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS)
            {
                float4 diffuse = float4(color.r, color.g, color.b, pMaterial->getBaseColor().a);
                pMaterial->setBaseColor(diffuse);
            }

            // Specular color
            if (pAiMaterial->Get(AI_MATKEY_COLOR_SPECULAR, color) == AI_SUCCESS)
            {
                float4 specular = float4(color.r, color.g, color.b, pMaterial->getSpecularParams().a);
                pMaterial->setSpecularParams(specular);
            }

            // Emissive color
            if (pAiMaterial->Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS)
            {
                float3 emissive = float3(color.r, color.g, color.b);
                pMaterial->setEmissiveColor(emissive);
            }

            // Double-Sided
            int isDoubleSided;
            if (pAiMaterial->Get(AI_MATKEY_TWOSIDED, isDoubleSided) == AI_SUCCESS)
            {
                pMaterial->setDoubleSided((isDoubleSided != 0));
            }

            // Handle GLTF2 PBR materials
            if (importMode == ImportMode::GLTF2)
            {
                if (pAiMaterial->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_BASE_COLOR_FACTOR, color) == AI_SUCCESS)
                {
                    float4 baseColor = float4(color.r, color.g, color.b, pMaterial->getBaseColor().a);
                    pMaterial->setBaseColor(baseColor);
                }

                float4 specularParams = pMaterial->getSpecularParams();

                float metallic;
                if (pAiMaterial->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLIC_FACTOR, metallic) == AI_SUCCESS)
                {
                    specularParams.b = metallic;
                }

                float roughness;
                if (pAiMaterial->Get(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS)
                {
                    specularParams.g = roughness;
                }

                pMaterial->setSpecularParams(specularParams);
            }

            // Parse the information contained in the name
            // Tokens following a '.' are interpreted as special flags
            auto nameVec = splitString(nameStr, ".");
            if (nameVec.size() > 1)
            {
                for (size_t i = 1; i < nameVec.size(); i++)
                {
                    std::string str = nameVec[i];
                    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
                    if (str == "doublesided") pMaterial->setDoubleSided(true);
                    else logWarning("AssimpImporter: Material '{}' has an unknown material property: '{}'.", nameStr, nameVec[i]);
                }
            }

            // Use scalar opacity value for controlling specular transmission
            // TODO: Remove this workaround when we have a better way to define materials.
            if (opacity < 1.f)
            {
                pMaterial->setSpecularTransmission(1.f - opacity);
            }

            return pMaterial;
        }

        void createAllMaterials(ImporterData& data, const std::filesystem::path& searchPath, ImportMode importMode)
        {
            for (uint32_t i = 0; i < data.pScene->mNumMaterials; i++)
            {
                const aiMaterial* pAiMaterial = data.pScene->mMaterials[i];
                data.materialMap[i] = createMaterial(data, pAiMaterial, searchPath, importMode);
            }
        }

        BoneMeshMap createBoneMap(const aiScene* pScene)
        {
            BoneMeshMap boneMap;

            for (uint32_t meshID = 0; meshID < pScene->mNumMeshes; meshID++)
            {
                const aiMesh* pMesh = pScene->mMeshes[meshID];
                for (uint32_t boneID = 0; boneID < pMesh->mNumBones; boneID++)
                {
                    boneMap[pMesh->mBones[boneID]->mName.C_Str()].push_back(meshID);
                }
            }

            return boneMap;
        }

        MeshInstanceList countMeshInstances(const aiScene* pScene)
        {
            MeshInstanceList meshInstances(pScene->mNumMeshes);

            std::function<void(const aiNode*)> countNodeMeshs = [&](const aiNode* pNode)
            {
                for (uint32_t i = 0; i < pNode->mNumMeshes; i++)
                {
                    meshInstances[pNode->mMeshes[i]].push_back(pNode);
                }

                for (uint32_t i = 0; i < pNode->mNumChildren; i++)
                {
                    countNodeMeshs(pNode->mChildren[i]);
                }
            };
            countNodeMeshs(pScene->mRootNode);

            return meshInstances;
        }

        void validateBones(ImporterData& data)
        {
            // Make sure that each bone is only affecting a single mesh.
            // Our skinning system depends on that, because we apply the inverse world transformation to blended vertices. We do that because apparently ASSIMP's bone matrices are pre-multiplied with the final world transform
            // which results in the world-space blended-vertices, but we'd like them to be in local-space
            BoneMeshMap boneMap = createBoneMap(data.pScene);
            MeshInstanceList meshInstances = countMeshInstances(data.pScene);

            for (auto& b : boneMap)
            {
                for (uint32_t i = 0; i < b.second.size(); i++)
                {
                    if (meshInstances[b.second[i]].size() != 1)
                    {
                        throw ImporterError(data.path, "Bone {} references a mesh with multiple instances.", b.first);
                    }

                    if (i > 0 && meshInstances[b.second[i]][0]->mTransformation != meshInstances[b.second[i - 1]][0]->mTransformation)
                    {
                        throw ImporterError(data.path, "Bone {} is contained within mesh instances with different world matrices.", b.first);
                    }
                }
            }
        }

        void validateScene(ImporterData& data)
        {
            if (data.pScene->mNumTextures > 0)
            {
                logWarning("AssimpImporter: Scene has {} embedded textures which Falcor doesn't load.", data.pScene->mNumTextures);
            }

            validateBones(data);
        }
    }

    void AssimpImporter::import(const std::filesystem::path& path, SceneBuilder& builder, const SceneBuilder::InstanceMatrices& instances, const Dictionary& dict)
    {
        TimeReport timeReport;

        std::filesystem::path fullPath;
        if (!findFileInDataDirectories(path, fullPath))
        {
            throw ImporterError(path, "File not found.");
        }

        const SceneBuilder::Flags builderFlags = builder.getFlags();
        uint32_t assimpFlags = aiProcess_FlipUVs;

		// thomasneff: these were uncommented to ensure parity between Falcor and comparison/baseline engines (like SAS)
		// 			   PSAO should work fine with these enabled.
        //assimpFlags &= ~(aiProcess_CalcTangentSpace); // Never use Assimp's tangent gen code
        //assimpFlags &= ~(aiProcess_FindDegenerates); // Avoid converting degenerated triangles to lines
        //assimpFlags &= ~(aiProcess_OptimizeGraph); // Never use as it doesn't handle transforms with negative determinants
        //assimpFlags &= ~(aiProcess_RemoveRedundantMaterials); // Avoid merging materials as it doesn't load all fields we care about, we merge in 'SceneBuilder' instead.
        //assimpFlags &= ~(aiProcess_SplitLargeMeshes); // Avoid splitting large meshes
        //if (is_set(builderFlags, SceneBuilder::Flags::DontMergeMeshes)) assimpFlags &= ~aiProcess_OptimizeMeshes; // Avoid merging original meshes

        // Configure importer to remove vertex components we don't support.
        // It'll load faster and helps 'aiProcess_JoinIdenticalVertices' find identical vertices.
        int removeFlags = aiComponent_COLORS;
        //for (uint32_t uvLayer = 1; uvLayer < AI_MAX_NUMBER_OF_TEXTURECOORDS; uvLayer++) removeFlags |= aiComponent_TEXCOORDSn(uvLayer);
        if (!is_set(builderFlags, SceneBuilder::Flags::UseOriginalTangentSpace)) removeFlags |= aiComponent_TANGENTS_AND_BITANGENTS;

        Assimp::Importer importer;
        importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, removeFlags);

        const aiScene* pScene = importer.ReadFile(fullPath.string().c_str(), assimpFlags);
        if (!pScene) throw ImporterError(path, "Failed to open scene: {}", importer.GetErrorString());
        timeReport.measure("Loading asset file");

        ImporterData data(path, pScene, builder, instances);

        validateScene(data);
        timeReport.measure("Verifying scene");

        // Extract the folder name
        auto searchPath = fullPath.parent_path();

        // Enable special treatment for obj and gltf files
        ImportMode importMode = ImportMode::Default;
        if (hasExtension(path, "obj")) importMode = ImportMode::OBJ;
        if (hasExtension(path, "gltf") || hasExtension(path, "glb")) importMode = ImportMode::GLTF2;

        createAllMaterials(data, searchPath, importMode);
        timeReport.measure("Creating materials");

        createSceneGraph(data);
        timeReport.measure("Creating scene graph");

        createMeshes(data);
        addMeshInstances(data, data.pScene->mRootNode);
        timeReport.measure("Creating meshes");

        createAnimations(data, importMode);
        timeReport.measure("Creating animations");

        createCameras(data, importMode);
        timeReport.measure("Creating cameras");

        createLights(data);
        timeReport.measure("Creating lights");

        timeReport.printToLog();
    }

    FALCOR_REGISTER_IMPORTER(
        AssimpImporter,
        Importer::ExtensionList({
            "fbx",
            "gltf",
            "obj",
            "dae",
            "x",
            "md5mesh",
            "ply",
            "3ds",
            "blend",
            "ase",
            "ifc",
            "xgl",
            "zgl",
            "dxf",
            "lwo",
            "lws",
            "lxo",
            "stl",
            "ac",
            "ms3d",
            "cob",
            "scn",
            "3d",
            "mdl",
            "mdl2",
            "pk3",
            "smd",
            "vta",
            "raw",
            "ter",
            "glb"
        })
    )
}
