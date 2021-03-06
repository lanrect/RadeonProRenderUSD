/************************************************************************
Copyright 2020 Advanced Micro Devices, Inc
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
************************************************************************/

#include "mesh.h"
#include "instancer.h"
#include "renderParam.h"
#include "material.h"
#include "materialAdapter.h"
#include "rprApi.h"

#include "pxr/imaging/pxOsd/tokens.h"
#include "pxr/imaging/pxOsd/subdivTags.h"

#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/sprim.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/hd/extComputationUtils.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec4f.h"

#include "pxr/usd/usdUtils/pipeline.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(HdRprMeshPrivateTokens,
    ((subdivisionLevel, "rpr:subdivisionLevel"))
);

HdRprMesh::HdRprMesh(SdfPath const& id, SdfPath const& instancerId)
    : HdMesh(id, instancerId) {

}

HdDirtyBits HdRprMesh::_PropagateDirtyBits(HdDirtyBits bits) const {
    return bits;
}

HdDirtyBits HdRprMesh::GetInitialDirtyBitsMask() const {
    // The initial dirty bits control what data is available on the first
    // run through _PopulateMesh(), so it should list every data item
    // that _PopluateMesh requests.
    int mask = HdChangeTracker::Clean
        | HdChangeTracker::DirtyPoints
        | HdChangeTracker::DirtyTopology
        | HdChangeTracker::DirtyTransform
        | HdChangeTracker::DirtyPrimvar
        | HdChangeTracker::DirtyNormals
        | HdChangeTracker::DirtyMaterialId
        | HdChangeTracker::DirtySubdivTags
        | HdChangeTracker::DirtyDisplayStyle
        | HdChangeTracker::DirtyVisibility
        | HdChangeTracker::DirtyInstancer
        | HdChangeTracker::DirtyInstanceIndex
        | HdChangeTracker::DirtyDoubleSided
        ;

    return (HdDirtyBits)mask;
}

void HdRprMesh::_InitRepr(TfToken const& reprName,
                          HdDirtyBits* dirtyBits) {
    TF_UNUSED(reprName);
    TF_UNUSED(dirtyBits);

    // No-op
}

template <typename T>
bool HdRprMesh::GetPrimvarData(TfToken const& name,
                               HdSceneDelegate* sceneDelegate,
                               std::map<HdInterpolation, HdPrimvarDescriptorVector> primvarDescsPerInterpolation,
                               VtArray<T>& out_data,
                               VtIntArray& out_indices) {
    out_data.clear();
    out_indices.clear();

    for (auto& primvarDescsEntry : primvarDescsPerInterpolation) {
        for (auto& pv : primvarDescsEntry.second) {
            if (pv.name == name) {
                auto value = GetPrimvar(sceneDelegate, name);
                if (value.IsHolding<VtArray<T>>()) {
                    out_data = value.UncheckedGet<VtArray<T>>();
                    if (primvarDescsEntry.first == HdInterpolationFaceVarying) {
                        out_indices.reserve(m_faceVertexIndices.size());
                        for (int i = 0; i < m_faceVertexIndices.size(); ++i) {
                            out_indices.push_back(i);
                        }
                    }
                    return true;
                }
                return false;
            }
        }
    }

    return false;
}
template bool HdRprMesh::GetPrimvarData<GfVec2f>(TfToken const&, HdSceneDelegate*, std::map<HdInterpolation, HdPrimvarDescriptorVector>, VtArray<GfVec2f>&, VtIntArray&);
template bool HdRprMesh::GetPrimvarData<GfVec3f>(TfToken const&, HdSceneDelegate*, std::map<HdInterpolation, HdPrimvarDescriptorVector>, VtArray<GfVec3f>&, VtIntArray&);

HdRprApiMaterial const* HdRprMesh::GetFallbackMaterial(HdSceneDelegate* sceneDelegate, HdRprApi* rprApi, HdDirtyBits dirtyBits) {
    if (m_fallbackMaterial && (dirtyBits & HdChangeTracker::DirtyPrimvar)) {
        rprApi->Release(m_fallbackMaterial);
        m_fallbackMaterial = nullptr;
    }

    if (!m_fallbackMaterial) {
        // XXX: Currently, displayColor is used as one color for whole mesh,
        // but it should be used as attribute per vertex/face.
        // RPR does not have such functionality, yet

        GfVec3f color(0.18f);

        HdPrimvarDescriptorVector primvars = sceneDelegate->GetPrimvarDescriptors(GetId(), HdInterpolationConstant);
        for (auto& pv : primvars) {
            if (pv.name == HdTokens->displayColor) {
                VtValue val = sceneDelegate->Get(GetId(), HdTokens->displayColor);
                if (val.IsHolding<VtVec3fArray>()) {
                    auto colors = val.UncheckedGet<VtVec3fArray>();
                    if (!colors.empty()) {
                        color = colors[0];
                    }
                    break;
                }
            }
        }

        auto matAdapter = MaterialAdapter(EMaterialType::COLOR, MaterialParams{ {HdRprMaterialTokens->color, VtValue(color)} });
        m_fallbackMaterial = rprApi->CreateMaterial(matAdapter);
    }

    return m_fallbackMaterial;
}

void HdRprMesh::Sync(HdSceneDelegate* sceneDelegate,
                     HdRenderParam* renderParam,
                     HdDirtyBits* dirtyBits,
                     TfToken const& reprName) {
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    auto rprRenderParam = static_cast<HdRprRenderParam*>(renderParam);
    auto rprApi = rprRenderParam->AcquireRprApiForEdit();

    SdfPath const& id = GetId();

    ////////////////////////////////////////////////////////////////////////
    // 1. Pull scene data.

    bool newMesh = false;

    bool pointsIsComputed = false;
    auto extComputationDescs = sceneDelegate->GetExtComputationPrimvarDescriptors(id, HdInterpolationVertex);
    for (auto& desc : extComputationDescs) {
        if (desc.name != HdTokens->points) {
            continue;
        }

        if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, desc.name)) {
            auto valueStore = HdExtComputationUtils::GetComputedPrimvarValues({desc}, sceneDelegate);
            auto pointValueIt = valueStore.find(desc.name);
            if (pointValueIt != valueStore.end()) {
                m_points = pointValueIt->second.Get<VtVec3fArray>();
                m_normalsValid = false;
                pointsIsComputed = true;

                newMesh = true;
            }
        }

        break;
    }

    if (!pointsIsComputed &&
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        VtValue pointsValue = sceneDelegate->Get(id, HdTokens->points);
        m_points = pointsValue.Get<VtVec3fArray>();
        m_normalsValid = false;

        newMesh = true;
    }

    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
        m_topology = GetMeshTopology(sceneDelegate);
        m_faceVertexCounts = m_topology.GetFaceVertexCounts();
        m_faceVertexIndices = m_topology.GetFaceVertexIndices();

        m_adjacencyValid = false;
        m_normalsValid = false;

        m_enableSubdiv = m_topology.GetScheme() == PxOsdOpenSubdivTokens->catmullClark;

        newMesh = true;
    }

    std::map<HdInterpolation, HdPrimvarDescriptorVector> primvarDescsPerInterpolation = {
        {HdInterpolationFaceVarying, sceneDelegate->GetPrimvarDescriptors(id, HdInterpolationFaceVarying)},
        {HdInterpolationVertex, sceneDelegate->GetPrimvarDescriptors(id, HdInterpolationVertex)},
        {HdInterpolationConstant, sceneDelegate->GetPrimvarDescriptors(id, HdInterpolationConstant)},
    };

    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->normals)) {
        m_authoredNormals = GetPrimvarData(HdTokens->normals, sceneDelegate, primvarDescsPerInterpolation, m_normals, m_normalIndices);

        newMesh = true;
    }

    auto stToken = UsdUtilsGetPrimaryUVSetName();
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, stToken)) {
        GetPrimvarData(stToken, sceneDelegate, primvarDescsPerInterpolation, m_uvs, m_uvIndices);

        newMesh = true;
    }

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        m_cachedMaterialId = sceneDelegate->GetMaterialId(id);
    }

    if (*dirtyBits & HdChangeTracker::DirtyVisibility) {
        _sharedData.visible = sceneDelegate->GetVisible(id);
    }

    if (*dirtyBits & HdChangeTracker::DirtyDoubleSided) {
        m_doublesided = sceneDelegate->GetDoubleSided(id);
    }

    ////////////////////////////////////////////////////////////////////////
    // 2. Resolve drawstyles

    bool isRefineLevelDirty = false;
    if (*dirtyBits & HdChangeTracker::DirtyDisplayStyle) {
        m_displayStyle = sceneDelegate->GetDisplayStyle(id);
        if (m_refineLevel != m_displayStyle.refineLevel) {
            isRefineLevelDirty = true;
            m_refineLevel = m_displayStyle.refineLevel;
        }
    }

    if (*dirtyBits & HdChangeTracker::DirtyPrimvar) {
        auto& constantPrimvars = primvarDescsPerInterpolation.at(HdInterpolationConstant);
        if (!constantPrimvars.empty()) {
            auto subdivisionLevelPrimvarIter = std::find_if(constantPrimvars.begin(), constantPrimvars.end(), [](HdPrimvarDescriptor const& desc) {
                return desc.name == HdRprMeshPrivateTokens->subdivisionLevel;
                });
            if (subdivisionLevelPrimvarIter != constantPrimvars.end()) {
                auto subdivisionLevelValue = sceneDelegate->Get(id, HdRprMeshPrivateTokens->subdivisionLevel);
                if (subdivisionLevelValue.IsHolding<int>()) {
                    int refineLevel = subdivisionLevelValue.UncheckedGet<int>();
                    if (m_refineLevel != refineLevel) {
                        isRefineLevelDirty = true;
                        m_refineLevel = refineLevel;
                    }
                } else {
                    auto typeName = subdivisionLevelValue.GetTypeName();
                    TF_WARN("[%s] %s: unexpected type. %s != int", id.GetText(), HdRprMeshPrivateTokens->subdivisionLevel.GetText(), typeName.c_str());
                }
            }
        }
    }

    m_smoothNormals = m_displayStyle.flatShadingEnabled;
    // Don't compute smooth normals on a refined mesh. They are implicitly smooth.
    m_smoothNormals = m_smoothNormals && !(m_enableSubdiv && m_refineLevel > 0);

    if (!m_authoredNormals && m_smoothNormals) {
        if (!m_adjacencyValid) {
            m_adjacency.BuildAdjacencyTable(&m_topology);
            m_adjacencyValid = true;
            m_normalsValid = false;
        }

        if (!m_normalsValid) {
            m_normals = Hd_SmoothNormals::ComputeSmoothNormals(&m_adjacency, m_points.size(), m_points.cdata());
            m_normalsValid = true;

            newMesh = true;
        }
    }

    bool updateTransform = newMesh;
    if (*dirtyBits & HdChangeTracker::DirtyTransform) {
        m_transform = GfMatrix4f(sceneDelegate->GetTransform(id));
        updateTransform = true;
    }

    ////////////////////////////////////////////////////////////////////////
    // 3. Create RPR meshes

    if (newMesh) {
        for (auto mesh : m_rprMeshes) {
            rprApi->Release(mesh);
        }
        for (auto instances : m_rprMeshInstances) {
            for (auto instance : instances) {
                rprApi->Release(instance);
            }
        }
        m_rprMeshInstances.clear();
        m_rprMeshes.clear();

        m_geomSubsets = m_topology.GetGeomSubsets();
        if (m_geomSubsets.empty()) {
            if (auto rprMesh = rprApi->CreateMesh(m_points, m_faceVertexIndices, m_normals, m_normalIndices, m_uvs, m_uvIndices, m_faceVertexCounts, m_topology.GetOrientation())) {
                m_rprMeshes.push_back(rprMesh);
            }
        } else {
            auto numFaces = m_faceVertexCounts.size();
            std::vector<bool> faceIsUnused(numFaces, true);
            size_t numUnusedFaces = faceIsUnused.size();
            for (auto const& subset : m_geomSubsets) {
                for (int index : subset.indices) {
                    if (TF_VERIFY(index < numFaces) && faceIsUnused[index]) {
                        faceIsUnused[index] = false;
                        numUnusedFaces--;
                    }
                }
            }
            // If we found any unused faces, build a final subset with those faces.
            // Use the material bound to the parent mesh.
            if (numUnusedFaces) {
                m_geomSubsets.push_back(HdGeomSubset());
                HdGeomSubset& unusedSubset = m_geomSubsets.back();
                unusedSubset.type = HdGeomSubset::TypeFaceSet;
                unusedSubset.id = id;
                unusedSubset.materialId = m_cachedMaterialId;
                unusedSubset.indices.resize(numUnusedFaces);
                size_t count = 0;
                for (size_t i = 0; i < faceIsUnused.size() && count < numUnusedFaces; ++i) {
                    if (faceIsUnused[i]) {
                        unusedSubset.indices[count] = i;
                        count++;
                    }
                }
            }

            // GeomSubset may reference face subset in any given order so we need to be able to
            //   randomly lookup face indexes but each face may be of an arbitrary number of vertices
            std::vector<int> indexesOffsetPrefixSum;
            indexesOffsetPrefixSum.reserve(m_faceVertexCounts.size());
            int offset = 0;
            for (auto numVerticesInFace : m_faceVertexCounts) {
                indexesOffsetPrefixSum.push_back(offset);
                offset += numVerticesInFace;
            }

            for (auto it = m_geomSubsets.begin(); it != m_geomSubsets.end();) {
                auto const& subset = *it;
                if (subset.type != HdGeomSubset::TypeFaceSet) {
                    TF_RUNTIME_ERROR("Unknown HdGeomSubset Type");
                    it = m_geomSubsets.erase(it);
                    continue;
                }

                VtVec3fArray subsetPoints;
                VtVec3fArray subsetNormals;
                VtVec2fArray subsetSt;
                VtIntArray subsetIndexes;
                VtIntArray subsetVertexPerFace;
                subsetVertexPerFace.reserve(subset.indices.size());

                int count = 0;
                for (auto faceIndex : subset.indices) {
                    int numVerticesInFace = m_faceVertexCounts[faceIndex];
                    subsetVertexPerFace.push_back(numVerticesInFace);

                    int faceIndexesOffset = indexesOffsetPrefixSum[faceIndex];

                    for (int i = 0; i < numVerticesInFace; ++i) {
                        subsetIndexes.push_back(count++);

                        int pointIndex = m_faceVertexIndices[faceIndexesOffset + i];
                        subsetPoints.push_back(m_points[pointIndex]);

                        if (!m_normals.empty()) {
                            int normalIndex = m_normalIndices.empty() ? pointIndex : m_normalIndices[faceIndexesOffset + i];
                            subsetNormals.push_back(m_normals[normalIndex]);
                        }

                        if (!m_uvs.empty()) {
                            int stIndex = m_uvIndices.empty() ? pointIndex : m_uvIndices[faceIndexesOffset + i];
                            subsetSt.push_back(m_uvs[stIndex]);
                        }
                    }
                }

                if (auto rprMesh = rprApi->CreateMesh(subsetPoints, subsetIndexes, subsetNormals, VtIntArray(), subsetSt, VtIntArray(), subsetVertexPerFace, m_topology.GetOrientation())) {
                    m_rprMeshes.push_back(rprMesh);
                    ++it;
                } else {
                    it = m_geomSubsets.erase(it);
                }
            }
        }
    }

    if (!m_rprMeshes.empty()) {
        if (newMesh || (*dirtyBits & HdChangeTracker::DirtySubdivTags)) {
            PxOsdSubdivTags subdivTags = sceneDelegate->GetSubdivTags(id);

            // XXX: RPR does not support this
            /*
            auto& cornerIndices = subdivTags.GetCornerIndices();
            auto& cornerSharpness = subdivTags.GetCornerWeights();
            if (!cornerIndices.empty() && !cornerSharpness.empty()) {

            }

            auto& creaseIndices = subdivTags.GetCreaseIndices();
            auto& creaseSharpness = subdivTags.GetCreaseWeights();
            if (!creaseIndices.empty() && !creaseSharpness.empty()) {

            }
            */

            auto vertexInterpolationRule = subdivTags.GetVertexInterpolationRule();
            for (auto& rprMesh : m_rprMeshes) {
                rprApi->SetMeshVertexInterpolationRule(rprMesh, vertexInterpolationRule);
            }
        }

        if (newMesh || isRefineLevelDirty) {
            for (auto& rprMesh : m_rprMeshes) {
                rprApi->SetMeshRefineLevel(rprMesh, m_enableSubdiv ? m_refineLevel : 0);
            }
        }

        if (newMesh || (*dirtyBits & HdChangeTracker::DirtyVisibility)) {
            for (auto& rprMesh : m_rprMeshes) {
                rprApi->SetMeshVisibility(rprMesh, _sharedData.visible);
            }
        }

        if (newMesh || (*dirtyBits & HdChangeTracker::DirtyMaterialId) ||
            (*dirtyBits & HdChangeTracker::DirtyDoubleSided) || // update twosided material node
            (*dirtyBits & HdChangeTracker::DirtyDisplayStyle) || isRefineLevelDirty) { // update displacement material
            auto getMeshMaterial = [sceneDelegate, rprApi, dirtyBits, this](SdfPath const& materialId) {
                auto material = static_cast<const HdRprMaterial*>(sceneDelegate->GetRenderIndex().GetSprim(HdPrimTypeTokens->material, materialId));
                if (material && material->GetRprMaterialObject()) {
                    return material->GetRprMaterialObject();
                } else {
                    return GetFallbackMaterial(sceneDelegate, rprApi, *dirtyBits);
                }
            };

            if (m_geomSubsets.empty()) {
                auto material = getMeshMaterial(m_cachedMaterialId);
                for (auto& mesh : m_rprMeshes) {
                    rprApi->SetMeshMaterial(mesh, material, m_doublesided, m_displayStyle.displacementEnabled);
                }
            } else {
                if (m_geomSubsets.size() == m_rprMeshes.size()) {
                    for (int i = 0; i < m_rprMeshes.size(); ++i) {
                        auto material = getMeshMaterial(m_geomSubsets[i].materialId);
                        rprApi->SetMeshMaterial(m_rprMeshes[i], material, m_doublesided, m_displayStyle.displacementEnabled);
                    }
                } else {
                    TF_CODING_ERROR("Unexpected number of meshes");
                }
            }
        }

        if (newMesh || (*dirtyBits & HdChangeTracker::DirtyInstancer)) {
            if (auto instancer = static_cast<HdRprInstancer*>(sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId()))) {
                auto transforms = instancer->ComputeTransforms(id);
                if (transforms.empty()) {
                    // Reset to state without instances
                    for (auto instances : m_rprMeshInstances) {
                        for (auto instance : instances) {
                            rprApi->Release(instance);
                        }
                    }
                    m_rprMeshInstances.clear();

                    for (int i = 0; i < m_rprMeshes.size(); ++i) {
                        rprApi->SetMeshVisibility(m_rprMeshes[i], _sharedData.visible);
                    }
                } else {
                    updateTransform = false;
                    GfMatrix4d meshTransform(m_transform);
                    for (auto& instanceTransform : transforms) {
                        instanceTransform = meshTransform * instanceTransform;
                    }

                    // Release excessive mesh instances if any
                    for (size_t i = m_rprMeshes.size(); i < m_rprMeshInstances.size(); ++i) {
                        for (auto instance : m_rprMeshInstances[i]) {
                            rprApi->Release(instance);
                        }
                    }

                    m_rprMeshInstances.resize(m_rprMeshes.size());

                    for (int i = 0; i < m_rprMeshes.size(); ++i) {
                        auto& meshInstances = m_rprMeshInstances[i];
                        if (meshInstances.size() != transforms.size()) {
                            if (meshInstances.size() > transforms.size()) {
                                for (size_t i = transforms.size(); i < meshInstances.size(); ++i) {
                                    rprApi->Release(meshInstances[i]);
                                }
                                meshInstances.resize(transforms.size());
                            } else {
                                for (int j = meshInstances.size(); j < transforms.size(); ++j) {
                                    meshInstances.push_back(rprApi->CreateMeshInstance(m_rprMeshes[i]));
                                }
                            }
                        }

                        for (int j = 0; j < transforms.size(); ++j) {
                            rprApi->SetTransform(meshInstances[j], GfMatrix4f(transforms[j]));
                        }

                        // Hide prototype
                        rprApi->SetMeshVisibility(m_rprMeshes[i], false);
                    }
                }
            }
        }

        if (updateTransform) {
            for (auto& rprMesh : m_rprMeshes) {
                rprApi->SetTransform(rprMesh, m_transform);
            }
        }
    }

    *dirtyBits = HdChangeTracker::Clean;
}

void HdRprMesh::Finalize(HdRenderParam* renderParam) {
    auto rprApi = static_cast<HdRprRenderParam*>(renderParam)->AcquireRprApiForEdit();

    for (auto mesh : m_rprMeshes) {
        rprApi->Release(mesh);
    }
    for (auto instances : m_rprMeshInstances) {
        for (auto instance : instances) {
            rprApi->Release(instance);
        }
    }
    m_rprMeshInstances.clear();
    m_rprMeshes.clear();

    rprApi->Release(m_fallbackMaterial);
    m_fallbackMaterial = nullptr;

    HdMesh::Finalize(renderParam);
}

PXR_NAMESPACE_CLOSE_SCOPE
