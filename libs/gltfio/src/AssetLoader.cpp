/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gltfio/AssetLoader.h>
#include <gltfio/MaterialProvider.h>

#include "FFilamentAsset.h"
#include "GltfEnums.h"

#include <filament/Box.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>

#include <math/mat4.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <utils/EntityManager.h>
#include <utils/Log.h>
#include <utils/NameComponentManager.h>

#include <tsl/robin_map.h>

#include <vector>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "math.h"
#include "upcast.h"

using namespace filament;
using namespace filament::math;
using namespace utils;

namespace gltfio {
namespace details {

// MeshCache
// ---------
// If a given glTF mesh is referenced by multiple glTF nodes, then it generates a separate Filament
// renderable for each of those nodes. All renderables generated by a given mesh share a common set
// of VertexBuffer and IndexBuffer objects. To achieve the sharing behavior, the loader maintains a
// small cache. The cache keys are glTF mesh definitions and the cache entries are lists of
// primitives, where a "primitive" is a reference to a Filament VertexBuffer and IndexBuffer.
struct Primitive {
    VertexBuffer* vertices = nullptr;
    IndexBuffer* indices = nullptr;
    Aabb aabb; // object-space bounding box
};
using MeshCache = tsl::robin_map<const cgltf_mesh*, std::vector<Primitive>>;

// MatInstanceCache
// ----------------
// Each glTF material definition corresponds to a single filament::MaterialInstance, which are
// cached here in the loader. The filament::Material objects that are used to create instances are
// cached in MaterialProvider. If a given glTF material is referenced by multiple glTF meshes, then
// their corresponding filament primitives will share the same Filament MaterialInstance and UvMap.
// The UvMap is a mapping from each texcoord slot in glTF to one of Filament's 2 texcoord sets.
struct MaterialEntry {
    MaterialInstance* instance;
    UvMap uvmap;
};
using MatInstanceCache = tsl::robin_map<intptr_t, MaterialEntry>;

// Sometimes a glTF bufferview includes unused data at the end (e.g. in skinning.gltf) so we need to
// compute the correct size of the vertex buffer. Filament automatically infers the size of
// driver-level vertex buffers from the attribute data (stride, count, offset) and clients are
// expected to avoid uploading data blobs that exceed this size. Since this information doesn't
// exist in the glTF we need to compute it manually. This is a bit of a cheat, cgltf_calc_size is
// private but its implementation file is available in this cpp file.
static uint32_t computeBindingSize(const cgltf_accessor* accessor){
    cgltf_size element_size = cgltf_calc_size(accessor->type, accessor->component_type);
    return uint32_t(accessor->stride * (accessor->count - 1) + element_size);
};

static uint32_t computeBindingOffset(const cgltf_accessor* accessor) {
    return uint32_t(accessor->offset + accessor->buffer_view->offset);
};

struct FAssetLoader : public AssetLoader {
    FAssetLoader(const AssetConfiguration& config) :
            mEntityManager(EntityManager::get()),
            mRenderableManager(config.engine->getRenderableManager()),
            mNameManager(config.names),
            mTransformManager(config.engine->getTransformManager()),
            mMaterials(config.materials),
            mEngine(config.engine) {}

    FFilamentAsset* createAssetFromJson(const uint8_t* bytes, uint32_t nbytes);
    FilamentAsset* createAssetFromBinary(const uint8_t* bytes, uint32_t nbytes);

    ~FAssetLoader() {
        delete mMaterials;
    }

    void destroyAsset(const FFilamentAsset* asset) {
        delete asset;
    }

    size_t getMaterialsCount() const noexcept {
        return mMaterials->getMaterialsCount();
    }

    const Material* const* getMaterials() const noexcept {
        return mMaterials->getMaterials();
    }

    void createAsset(const cgltf_data* srcAsset);
    void createEntity(const cgltf_node* node, Entity parent);
    void createRenderable(const cgltf_node* node, Entity entity);
    bool createPrimitive(const cgltf_primitive* inPrim, Primitive* outPrim, const UvMap& uvmap);
    MaterialInstance* createMaterialInstance(const cgltf_material* inputMat, UvMap* uvmap,
            bool vertexColor);
    void addTextureBinding(MaterialInstance* materialInstance, const char* parameterName,
            const cgltf_texture* srcTexture, bool srgb);
    void importSkinningData(Skin& dstSkin, const cgltf_skin& srcSkin);
    bool primitiveHasVertexColor(const cgltf_primitive* inPrim) const;

    EntityManager& mEntityManager;
    RenderableManager& mRenderableManager;
    NameComponentManager* mNameManager;
    TransformManager& mTransformManager;
    MaterialProvider* mMaterials;
    Engine* mEngine;

    // The loader owns a few transient mappings used only for the current asset being loaded.
    FFilamentAsset* mResult;
    MatInstanceCache mMatInstanceCache;
    MeshCache mMeshCache;
    bool mError = false;
    bool mDiagnosticsEnabled = false;
};

FILAMENT_UPCAST(AssetLoader)

} // namespace details

using namespace details;

FFilamentAsset* FAssetLoader::createAssetFromJson(const uint8_t* bytes, uint32_t nbytes) {
    cgltf_options options { cgltf_file_type_invalid };
    cgltf_data* sourceAsset;
    cgltf_result result = cgltf_parse(&options, bytes, nbytes, &sourceAsset);
    if (result != cgltf_result_success) {
        return nullptr;
    }
    createAsset(sourceAsset);
    return mResult;
}

FilamentAsset* FAssetLoader::createAssetFromBinary(const uint8_t* bytes, uint32_t nbytes) {

    // The cgltf library handles GLB efficiently by pointing all buffer views into the source data.
    // However, we wish our API to be simple and safe, allowing clients to free up their source blob
    // immediately, without worrying about when all the data has finished uploading asynchronously
    // to the GPU. To achieve this we create a copy of the source blob and stash it inside the
    // asset, asking cgltf to parse the copy. This allows us to free it at the correct time (i.e.
    // after all GPU uploads have completed). Although it incurs a copy, the added safety of this
    // API seems worthwhile.
    std::vector<uint8_t> glbdata(bytes, bytes + nbytes);

    cgltf_options options { cgltf_file_type_glb };
    cgltf_data* sourceAsset;
    cgltf_result result = cgltf_parse(&options, glbdata.data(), nbytes, &sourceAsset);
    if (result != cgltf_result_success) {
        return nullptr;
    }
    createAsset(sourceAsset);
    if (mResult) {
        glbdata.swap(mResult->mGlbData);
    }
    return mResult;
}

void FAssetLoader::createAsset(const cgltf_data* srcAsset) {
    mResult = new FFilamentAsset(mEngine);
    mResult->mSourceAsset = srcAsset;
    mResult->acquireSourceAsset();

    // If there is no default scene specified, then the default is the first one.
    // It is not an error for a glTF file to have zero scenes.
    const cgltf_scene* scene = srcAsset->scene ? srcAsset->scene : srcAsset->scenes;
    if (!scene) {
        return;
    }

    // Create a single root node with an identity transform as a convenience to the client.
    mResult->mRoot = mEntityManager.create();
    mTransformManager.create(mResult->mRoot);

    // One scene may have multiple root nodes. Recurse down and create an entity for each node.
    cgltf_node** nodes = scene->nodes;
    for (cgltf_size i = 0, len = scene->nodes_count; i < len; ++i) {
        const cgltf_node* root = nodes[i];
        createEntity(root, mResult->mRoot);
    }

    if (mError) {
        delete mResult;
        mResult = nullptr;
    }

    // Copy over joint lists (references to TransformManager components) and create buffer bindings
    // for inverseBindMatrices.
    mResult->mSkins.resize(srcAsset->skins_count);
    for (cgltf_size i = 0, len = srcAsset->skins_count; i < len; ++i) {
        importSkinningData(mResult->mSkins[i], srcAsset->skins[i]);
    }

    // For each skin, build a list of renderables that it affects.
    for (cgltf_size i = 0, len = srcAsset->nodes_count; i < len; ++i) {
        const cgltf_node& node = srcAsset->nodes[i];
        if (node.skin) {
            int skinIndex = node.skin - &srcAsset->skins[0];
            Entity entity = mResult->mNodeMap[&node];
            mResult->mSkins[skinIndex].targets.push_back(entity);
        }
    }

    // We're done with the import, so free up transient bookkeeping resources.
    mMatInstanceCache.clear();
    mMeshCache.clear();
    mError = false;
}

void FAssetLoader::createEntity(const cgltf_node* node, Entity parent) {
    Entity entity = mEntityManager.create();

    // Always create a transform component to reflect the original hierarchy.
    mat4f localTransform;
    if (node->has_matrix) {
        memcpy(&localTransform[0][0], &node->matrix[0], 16 * sizeof(float));
    } else {
        quatf* rotation = (quatf*) &node->rotation[0];
        float3* scale = (float3*) &node->scale[0];
        float3* translation = (float3*) &node->translation[0];
        localTransform = composeMatrix(*translation, *rotation, *scale);
    }

    auto parentTransform = mTransformManager.getInstance(parent);
    mTransformManager.create(entity, parentTransform, localTransform);

    // Update the asset's entity list and private node mapping.
    mResult->mEntities.push_back(entity);
    mResult->mNodeMap[node] = entity;

    // If the node has a mesh, then create a renderable component.
    if (node->mesh) {
        createRenderable(node, entity);
     }

    for (cgltf_size i = 0, len = node->children_count; i < len; ++i) {
        createEntity(node->children[i], entity);
    }
}

void FAssetLoader::createRenderable(const cgltf_node* node, Entity entity) {
    const cgltf_mesh* mesh = node->mesh;

    // Compute the transform relative to the root.
    auto thisTransform = mTransformManager.getInstance(entity);
    mat4f worldTransform = mTransformManager.getWorldTransform(thisTransform);

    cgltf_size nprims = mesh->primitives_count;
    RenderableManager::Builder builder(nprims);

    // If the mesh is already loaded, obtain the list of Filament VertexBuffer / IndexBuffer
    // objects that were already generated, otherwise allocate a new list of null pointers.
    auto iter = mMeshCache.find(mesh);
    if (iter == mMeshCache.end()) {
        mMeshCache[mesh].resize(nprims);
    }
    Primitive* outputPrim = mMeshCache[mesh].data();
    const cgltf_primitive* inputPrim = &mesh->primitives[0];

    if (mNameManager && mesh->name) {
        mNameManager->addComponent(entity);
        mNameManager->setName(mNameManager->getInstance(entity), mesh->name);
    }

    Aabb aabb;

    // For each prim, create a Filament VertexBuffer, IndexBuffer, and MaterialInstance.
    for (cgltf_size index = 0; index < nprims; ++index, ++outputPrim, ++inputPrim) {
        RenderableManager::PrimitiveType primType;
        if (!getPrimitiveType(inputPrim->type, &primType)) {
            slog.e << "Unsupported primitive type." << io::endl;
        }

        // Create a material instance for this primitive or fetch one from the cache.
        UvMap uvmap {};
        bool hasVertexColor = primitiveHasVertexColor(inputPrim);
        MaterialInstance* mi = createMaterialInstance(inputPrim->material, &uvmap, hasVertexColor);
        builder.material(index, mi);

        // Create a Filament VertexBuffer and IndexBuffer for this prim if we haven't already.
        if (!outputPrim->vertices && !createPrimitive(inputPrim, outputPrim, uvmap)) {
            mError = true;
            continue;
        }

        // Expand the object-space bounding box.
        aabb.min = min(outputPrim->aabb.min, aabb.min);
        aabb.max = max(outputPrim->aabb.max, aabb.max);

        // We are not using the optional offset, minIndex, maxIndex, and count arguments when
        // calling geometry() on the builder. It appears that the glTF spec does not have
        // facilities for these parameters, which is not a huge loss since some of the buffer
        // view and accessor features already have this functionality.
        builder.geometry(index, primType, outputPrim->vertices, outputPrim->indices);
    }

    // Transform all eight corners of the bounding box and find the new AABB.
    float3 a = (worldTransform * float4(aabb.min.x, aabb.min.y, aabb.min.z, 1.0)).xyz;
    float3 b = (worldTransform * float4(aabb.min.x, aabb.min.y, aabb.max.z, 1.0)).xyz;
    float3 c = (worldTransform * float4(aabb.min.x, aabb.max.y, aabb.min.z, 1.0)).xyz;
    float3 d = (worldTransform * float4(aabb.min.x, aabb.max.y, aabb.max.z, 1.0)).xyz;
    float3 e = (worldTransform * float4(aabb.max.x, aabb.min.y, aabb.min.z, 1.0)).xyz;
    float3 f = (worldTransform * float4(aabb.max.x, aabb.min.y, aabb.max.z, 1.0)).xyz;
    float3 g = (worldTransform * float4(aabb.max.x, aabb.max.y, aabb.min.z, 1.0)).xyz;
    float3 h = (worldTransform * float4(aabb.max.x, aabb.max.y, aabb.max.z, 1.0)).xyz;
    float3 minpt = min(min(min(min(min(min(min(a, b), c), d), e), f), g), h);
    float3 maxpt = max(max(max(max(max(max(max(a, b), c), d), e), f), g), h);

    // Expand the world-space bounding box.
    mResult->mBoundingBox.min = min(mResult->mBoundingBox.min, minpt);
    mResult->mBoundingBox.max = max(mResult->mBoundingBox.max, maxpt);

    if (node->skin) {
       builder.skinning(node->skin->joints_count);
    }

    builder
        .boundingBox(Box().set(aabb.min, aabb.max))
        .culling(true)
        .castShadows(true)
        .receiveShadows(true)
        .build(*mEngine, entity);

    // TODO: support vertex morphing by honoring mesh->weights and mesh->weight_count.
}

bool FAssetLoader::createPrimitive(const cgltf_primitive* inPrim, Primitive* outPrim,
        const UvMap& uvmap) {

    // In glTF, each primitive may or may not have an index buffer. If a primitive does not have an
    // index buffer, we ask the ResourceLoader to generate a trivial index buffer.
    IndexBuffer* indices;
    const cgltf_accessor* indicesAccessor = inPrim->indices;
    if (indicesAccessor) {
        IndexBuffer::Builder ibb;
        ibb.indexCount(indicesAccessor->count);
        IndexBuffer::IndexType indexType;
        if (!getIndexType(indicesAccessor->component_type, &indexType)) {
            utils::slog.e << "Unrecognized index type." << utils::io::endl;
            return false;
        }
        ibb.bufferType(indexType);
        indices = ibb.build(*mEngine);
        const cgltf_buffer_view* bv = indicesAccessor->buffer_view;
        mResult->mBufferBindings.emplace_back(BufferBinding {
            .uri = bv->buffer->uri,
            .totalSize = uint32_t(bv->buffer->size),
            .offset = computeBindingOffset(indicesAccessor),
            .size = computeBindingSize(indicesAccessor),
            .data = &bv->buffer->data,
            .indexBuffer = indices,
            .convertBytesToShorts = indicesAccessor->component_type == cgltf_component_type_r_8u,
            .generateTrivialIndices = false
        });
    } else {
        const cgltf_size vertexCount = inPrim->attributes[0].data->count;
        indices = IndexBuffer::Builder()
            .indexCount(vertexCount)
            .bufferType(IndexBuffer::IndexType::UINT)
            .build(*mEngine);
        mResult->mBufferBindings.emplace_back(BufferBinding {
            .indexBuffer = indices,
            .size = uint32_t(vertexCount * sizeof(uint32_t)),
            .generateTrivialIndices = true
        });
    }
    mResult->mIndexBuffers.push_back(indices);

    VertexBuffer::Builder vbb;

    int slot = 0;
    bool hasUv0 = false, hasUv1 = false, hasVertexColor = false;
    uint32_t vertexCount = 0;
    for (cgltf_size aindex = 0; aindex < inPrim->attributes_count; aindex++) {
        const cgltf_attribute& inputAttribute = inPrim->attributes[aindex];
        const cgltf_accessor* inputAccessor = inputAttribute.data;

        // At a minimum, surface orientation requires normals to be present in the source data.
        // Here we re-purpose the normals slot to point to the quats that get computed later.
        if (inputAttribute.type == cgltf_attribute_type_normal) {
            vbb.attribute(VertexAttribute::TANGENTS, slot++, VertexBuffer::AttributeType::SHORT4);
            vbb.normalized(VertexAttribute::TANGENTS);
            continue;
        }

        // The glTF tangent data is ignored here, but honored in ResourceLoader.
        if (inputAttribute.type == cgltf_attribute_type_tangent) {
            continue;
        }

        if (inputAttribute.type == cgltf_attribute_type_color) {
            hasVertexColor = true;
        }

        // Translate the cgltf attribute enum into a Filament enum and ignore all uv sets
        // that do not have entries in the mapping table.
        VertexAttribute semantic;
        if (!getVertexAttrType(inputAttribute.type, &semantic)) {
            utils::slog.e << "Unrecognized vertex semantic." << utils::io::endl;
            return false;
        }
        UvSet uvset = uvmap[inputAttribute.index];
        if (inputAttribute.type == cgltf_attribute_type_texcoord) {
            switch (uvset) {
                case UV0:
                    semantic = VertexAttribute::UV0;
                    hasUv0 = true;
                    break;
                case UV1:
                    semantic = VertexAttribute::UV1;
                    hasUv1 = true;
                    break;
                case UNUSED:
                    // It is perfectly acceptable to drop unused texture coordinate sets. In fact
                    // this can occur quite frequently, e.g. if the material has attached textures.
                    continue;
            }
        }

        vertexCount = inputAccessor->count;

        // The positions accessor is required to have min/max properties, use them to expand
        // the bounding box for this primitive.
        if (inputAttribute.type == cgltf_attribute_type_position) {
            const float* minp = &inputAccessor->min[0];
            const float* maxp = &inputAccessor->max[0];
            outPrim->aabb.min = min(outPrim->aabb.min, float3(minp[0], minp[1], minp[2]));
            outPrim->aabb.max = max(outPrim->aabb.max, float3(maxp[0], maxp[1], maxp[2]));
        }

        VertexBuffer::AttributeType atype;
        if (!getElementType(inputAccessor->type, inputAccessor->component_type, &atype)) {
            slog.e << "Unsupported accessor type." << io::endl;
            return false;
        }

        if (inputAccessor->is_sparse) {
            slog.e << "Sparse accessors not yet supported." << io::endl;
            return false;
        }

        // The cgltf library provides a stride value for all accessors, even though they do not
        // exist in the glTF file. It is computed from the type and the stride of the buffer view.
        // As a convenience, cgltf also replaces zero (default) stride with the actual stride.
        vbb.attribute(semantic, slot++, atype, 0, inputAccessor->stride);

        if (inputAccessor->normalized) {
            vbb.normalized(semantic);
        }
    }

    vbb.vertexCount(vertexCount);

    // If an ubershader is used, then we provide a single dummy buffer for all unfulfilled vertex
    // requirements. The color data should be a sequence of normalized UBYTE4, so dummy UVs are
    // USHORT2 to make the sizes match.
    bool needsDummyData = false;
    if (mMaterials->getSource() == LOAD_UBERSHADERS) {
        if (!hasUv0) {
            needsDummyData = true;
            vbb.attribute(VertexAttribute::UV0, slot, VertexBuffer::AttributeType::USHORT2);
        }
        if (!hasUv1) {
            needsDummyData = true;
            vbb.attribute(VertexAttribute::UV1, slot, VertexBuffer::AttributeType::USHORT2);
        }
        if (!hasVertexColor) {
            needsDummyData = true;
            vbb.attribute(VertexAttribute::COLOR, slot, VertexBuffer::AttributeType::UBYTE4);
            vbb.normalized(VertexAttribute::COLOR);
        }
    if (needsDummyData) {
        slot++;
    }

    int bufferCount = slot;
    vbb.bufferCount(bufferCount);

    VertexBuffer* vertices = mResult->mPrimMap[inPrim] = vbb.build(*mEngine);
    mResult->mVertexBuffers.push_back(vertices);

    slot = 0;
    for (cgltf_size aindex = 0; aindex < inPrim->attributes_count; aindex++) {
        const cgltf_attribute& inputAttribute = inPrim->attributes[aindex];
        const cgltf_accessor* inputAccessor = inputAttribute.data;
        const cgltf_buffer_view* bv = inputAccessor->buffer_view;
        if (inputAttribute.type == cgltf_attribute_type_tangent ||
                (inputAttribute.type == cgltf_attribute_type_texcoord &&
                uvmap[inputAttribute.index] == UNUSED)) {
            continue;
        }
        if (inputAttribute.type == cgltf_attribute_type_normal) {
            mResult->mBufferBindings.push_back({
                .uri = bv->buffer->uri,
                .totalSize = uint32_t(bv->buffer->size),
                .bufferIndex = uint8_t(slot++),
                .vertexBuffer = vertices,
                .indexBuffer = nullptr,
                .convertBytesToShorts = false,
                .generateTrivialIndices = false,
                .generateDummyData = false,
                .generateTangents = true,
            });
            continue;
        }
        mResult->mBufferBindings.push_back({
            .uri = bv->buffer->uri,
            .totalSize = uint32_t(bv->buffer->size),
            .bufferIndex = uint8_t(slot++),
            .offset = computeBindingOffset(inputAccessor),
            .size = computeBindingSize(inputAccessor),
            .data = &bv->buffer->data,
            .vertexBuffer = vertices,
            .indexBuffer = nullptr,
            .convertBytesToShorts = false,
            .generateTrivialIndices = false,
            .generateDummyData = false,
            .generateTangents = false
        });
    }

    if (needsDummyData) {
        mResult->mBufferBindings.push_back({
            .uri = "",
            .totalSize = uint32_t(sizeof(ubyte4) * vertexCount),
            .bufferIndex = uint8_t(slot++),
            .offset = 0,
            .size = uint32_t(sizeof(ubyte4) * vertexCount),
            .data = nullptr,
            .vertexBuffer = vertices,
            .indexBuffer = nullptr,
            .convertBytesToShorts = false,
            .generateTrivialIndices = false,
            .generateDummyData = true
        });
    }

    assert(bufferCount == slot);

    outPrim->indices = indices;
    outPrim->vertices = vertices;
    return true;
}

MaterialInstance* FAssetLoader::createMaterialInstance(const cgltf_material* inputMat,
        UvMap* uvmap, bool vertexColor) {
    intptr_t key = ((intptr_t) inputMat) ^ (vertexColor ? 1 : 0);
    auto iter = mMatInstanceCache.find(key);
    if (iter != mMatInstanceCache.end()) {
        *uvmap = iter->second.uvmap;
        return iter->second.instance;
    }

    // The default glTF material is non-lit black.
    if (inputMat == nullptr) {
        MaterialKey matkey {
            .unlit = true
        };
        MaterialInstance* mi = mMaterials->createMaterialInstance(&matkey, uvmap, "default");
        mResult->mMaterialInstances.push_back(mi);
        mMatInstanceCache[0] = {mi, *uvmap};
        return mi;
    }

    auto mrConfig = inputMat->pbr_metallic_roughness;
    auto sgConfig = inputMat->pbr_specular_glossiness;

    bool hasTextureTransforms =
        sgConfig.diffuse_texture.has_transform ||
        sgConfig.specular_glossiness_texture.has_transform ||
        mrConfig.base_color_texture.has_transform ||
        mrConfig.metallic_roughness_texture.has_transform ||
        inputMat->normal_texture.has_transform ||
        inputMat->occlusion_texture.has_transform ||
        inputMat->emissive_texture.has_transform;

    cgltf_texture_view baseColorTexture = mrConfig.base_color_texture;
    cgltf_texture_view metallicRoughnessTexture = mrConfig.metallic_roughness_texture;

    MaterialKey matkey {
        .doubleSided = (bool) inputMat->double_sided,
        .unlit = (bool) inputMat->unlit,
        .hasVertexColors = vertexColor,
        .hasBaseColorTexture = baseColorTexture.texture,
        .hasNormalTexture = inputMat->normal_texture.texture,
        .hasOcclusionTexture = inputMat->occlusion_texture.texture,
        .hasEmissiveTexture = inputMat->emissive_texture.texture,
        .useSpecularGlossiness = false,
        .alphaMode = AlphaMode::OPAQUE,
        .enableDiagnostics = mDiagnosticsEnabled,
        .hasMetallicRoughnessTexture = metallicRoughnessTexture.texture,
        .metallicRoughnessUV = (uint8_t) metallicRoughnessTexture.texcoord,
        .baseColorUV = (uint8_t) baseColorTexture.texcoord,
        .emissiveUV = (uint8_t) inputMat->emissive_texture.texcoord,
        .aoUV = (uint8_t) inputMat->occlusion_texture.texcoord,
        .normalUV = (uint8_t) inputMat->normal_texture.texcoord,
        .hasTextureTransforms = hasTextureTransforms,
    };

    if (inputMat->has_pbr_specular_glossiness) {
        matkey.useSpecularGlossiness = true;
        if (sgConfig.diffuse_texture.texture) {
            baseColorTexture = sgConfig.diffuse_texture;
            matkey.hasBaseColorTexture = true;
            matkey.baseColorUV = (uint8_t) baseColorTexture.texcoord;
        }
        if (sgConfig.specular_glossiness_texture.texture) {
            metallicRoughnessTexture = sgConfig.specular_glossiness_texture;
            matkey.hasSpecularGlossinessTexture = true;
            matkey.specularGlossinessUV = (uint8_t) metallicRoughnessTexture.texcoord;
        }
    }

    switch (inputMat->alpha_mode) {
        case cgltf_alpha_mode_opaque:
            matkey.alphaMode = AlphaMode::OPAQUE;
            break;
        case cgltf_alpha_mode_mask:
            matkey.alphaMode = AlphaMode::MASK;
            break;
        case cgltf_alpha_mode_blend:
            matkey.alphaMode = AlphaMode::BLEND;
            break;
    }

    // This not only creates a material instance, it modifies the material key according to our
    // rendering constraints. For example, Filament only supports 2 sets of texture coordinates.
    MaterialInstance* mi = mMaterials->createMaterialInstance(&matkey, uvmap, inputMat->name);
    mResult->mMaterialInstances.push_back(mi);

    if (inputMat->alpha_mode == cgltf_alpha_mode_mask) {
        mi->setMaskThreshold(inputMat->alpha_cutoff);
    }

    const float* e = inputMat->emissive_factor;
    mi->setParameter("emissiveFactor", float3(e[0], e[1], e[2]));

    const float* c = mrConfig.base_color_factor;
    mi->setParameter("baseColorFactor", float4(c[0], c[1], c[2], c[3]));
    mi->setParameter("metallicFactor", mrConfig.metallic_factor);
    mi->setParameter("roughnessFactor", mrConfig.roughness_factor);

    if (matkey.useSpecularGlossiness) {
        const float* df = sgConfig.diffuse_factor;
        const float* sf = sgConfig.specular_factor;
        mi->setParameter("baseColorFactor", float4(df[0], df[1], df[2], df[3]));
        mi->setParameter("specularFactor", float3(sf[0], sf[1], sf[2]));
        mi->setParameter("glossinessFactor", sgConfig.glossiness_factor);
    }

    if (matkey.hasBaseColorTexture) {
        addTextureBinding(mi, "baseColorMap", baseColorTexture.texture, true);
        if (matkey.hasTextureTransforms) {
            const cgltf_texture_transform& uvt = baseColorTexture.transform;
            auto uvmat = matrixFromUvTransform(uvt.offset, uvt.rotation, uvt.scale);
            mi->setParameter("baseColorUvMatrix", uvmat);
        }
    }

    if (matkey.hasMetallicRoughnessTexture) {
        // The "metallicRoughnessMap" is actually a specular-glossiness map when the extension is
        // enabled. Note that KHR_materials_pbrSpecularGlossiness specifies that diffuseTexture and
        // specularGlossinessTexture are both sRGB, whereas the core glTF spec stipulates that
        // metallicRoughness is not sRGB.
        bool srgb = inputMat->has_pbr_specular_glossiness;
        addTextureBinding(mi, "metallicRoughnessMap", metallicRoughnessTexture.texture, srgb);
        if (matkey.hasTextureTransforms) {
            const cgltf_texture_transform& uvt = metallicRoughnessTexture.transform;
            auto uvmat = matrixFromUvTransform(uvt.offset, uvt.rotation, uvt.scale);
            mi->setParameter("metallicRoughnessUvMatrix", uvmat);
        }
    }

    if (matkey.hasNormalTexture) {
        addTextureBinding(mi, "normalMap", inputMat->normal_texture.texture, false);
        if (matkey.hasTextureTransforms) {
            const cgltf_texture_transform& uvt = inputMat->normal_texture.transform;
            auto uvmat = matrixFromUvTransform(uvt.offset, uvt.rotation, uvt.scale);
            mi->setParameter("normalUvMatrix", uvmat);
        }
        mi->setParameter("normalScale", inputMat->normal_texture.scale);
    } else {
        mi->setParameter("normalScale", 1.0f);
    }

    if (matkey.hasOcclusionTexture) {
        addTextureBinding(mi, "occlusionMap", inputMat->occlusion_texture.texture, false);
        if (matkey.hasTextureTransforms) {
            const cgltf_texture_transform& uvt = inputMat->occlusion_texture.transform;
            auto uvmat = matrixFromUvTransform(uvt.offset, uvt.rotation, uvt.scale);
            mi->setParameter("occlusionUvMatrix", uvmat);
        }
        mi->setParameter("aoStrength", inputMat->occlusion_texture.scale);
    } else {
        mi->setParameter("aoStrength", 1.0f);
    }

    if (matkey.hasEmissiveTexture) {
        addTextureBinding(mi, "emissiveMap", inputMat->emissive_texture.texture, true);
        if (matkey.hasTextureTransforms) {
            const cgltf_texture_transform& uvt = inputMat->emissive_texture.transform;
            auto uvmat = matrixFromUvTransform(uvt.offset, uvt.rotation, uvt.scale);
            mi->setParameter("emissiveUvMatrix", uvmat);
        }
    }

    mMatInstanceCache[key] = {mi, *uvmap};
    return mi;
}

void FAssetLoader::addTextureBinding(MaterialInstance* materialInstance, const char* parameterName,
        const cgltf_texture* srcTexture, bool srgb) {
    if (!srcTexture->image) {
        slog.w << "Texture is missing image (" << srcTexture->name << ")." << io::endl;
        return;
    }
    TextureSampler dstSampler;
    auto srcSampler = srcTexture->sampler;
    if (srcSampler) {
        dstSampler.setWrapModeS(getWrapMode(srcSampler->wrap_s));
        dstSampler.setWrapModeT(getWrapMode(srcSampler->wrap_t));
        dstSampler.setMagFilter(getMagFilter(srcSampler->mag_filter));
        dstSampler.setMinFilter(getMinFilter(srcSampler->min_filter));
    } else {
        // These defaults are stipulated by the spec:
        dstSampler.setWrapModeS(TextureSampler::WrapMode::REPEAT);
        dstSampler.setWrapModeT(TextureSampler::WrapMode::REPEAT);

        // These defaults are up the implementation but since we generate mipmaps unconditionally,
        // we might as well use them. In practice the conformance models look awful without
        // using mipmapping by default.
        dstSampler.setMagFilter(TextureSampler::MagFilter::LINEAR);
        dstSampler.setMinFilter(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR);
    }
    auto bv = srcTexture->image->buffer_view;
    mResult->mTextureBindings.push_back(TextureBinding {
        .uri = srcTexture->image->uri,
        .totalSize = uint32_t(bv ? bv->size : 0),
        .mimeType = srcTexture->image->mime_type,
        .data = bv ? &bv->buffer->data : nullptr,
        .offset = bv ? bv->offset : 0,
        .materialInstance = materialInstance,
        .materialParameter = parameterName,
        .sampler = dstSampler,
        .srgb = srgb
    });
}

void FAssetLoader::importSkinningData(Skin& dstSkin, const cgltf_skin& srcSkin) {
    if (srcSkin.name) {
        dstSkin.name = srcSkin.name;
    }
    dstSkin.joints.resize(srcSkin.joints_count);
    const auto& nodeMap = mResult->mNodeMap;
    for (cgltf_size i = 0, len = srcSkin.joints_count; i < len; ++i) {
        dstSkin.joints[i] = nodeMap.at(srcSkin.joints[i]);
    }
}

bool FAssetLoader::primitiveHasVertexColor(const cgltf_primitive* inPrim) const {
    for (int slot = 0; slot < inPrim->attributes_count; slot++) {
        const cgltf_attribute& inputAttribute = inPrim->attributes[slot];
        if (inputAttribute.type == cgltf_attribute_type_color) {
            return true;
        }
    }
    return false;
}

AssetLoader* AssetLoader::create(const AssetConfiguration& config) {
    return new FAssetLoader(config);
}

void AssetLoader::destroy(AssetLoader** loader) {
    delete *loader;
    *loader = nullptr;
}

FilamentAsset* AssetLoader::createAssetFromJson(uint8_t const* bytes, uint32_t nbytes) {
    return upcast(this)->createAssetFromJson(bytes, nbytes);
}

FilamentAsset* AssetLoader::createAssetFromBinary(uint8_t const* bytes, uint32_t nbytes) {
    return upcast(this)->createAssetFromBinary(bytes, nbytes);
}

FilamentAsset* AssetLoader::createAssetFromHandle(const void* handle) {
    const cgltf_data* sourceAsset = (const cgltf_data*) handle;
    upcast(this)->createAsset(sourceAsset);
    upcast(this)->mResult->mSharedSourceAsset = true;
    return upcast(this)->mResult;
}

void AssetLoader::enableDiagnostics(bool enable) {
    upcast(this)->mDiagnosticsEnabled = enable;
}

void AssetLoader::destroyAsset(const FilamentAsset* asset) {
    upcast(this)->destroyAsset(upcast(asset));
}

size_t AssetLoader::getMaterialsCount() const noexcept {
    return upcast(this)->getMaterialsCount();
}

const Material* const* AssetLoader::getMaterials() const noexcept {
    return upcast(this)->getMaterials();
}

} // namespace gltfio
