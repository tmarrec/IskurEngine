// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#include "World.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_NOEXCEPTION
#include <tiny_gltf.h>

#include "../common/Asserts.h"
#include "Camera.h"

namespace
{
void GetPrimitiveRecursion(Vector<SharedPtr<Primitive>>& primitives, const SharedPtr<Node>& node)
{
    if (node->mesh)
    {
        for (const SharedPtr<Primitive>& primitive : node->mesh->GetPrimitives())
        {
            primitives.Add(primitive);
        }
    }
    for (SharedPtr<Node>& childrenNode : node->nodes)
    {
        GetPrimitiveRecursion(primitives, childrenNode);
    }
}
} // namespace

World::World(const tinygltf::Model& model)
{
    for (const tinygltf::Scene& scene : model.scenes)
    {
        Scene newScene;
        for (const i32 nodeIdx : scene.nodes)
        {
            InitNodes(model, newScene, nodeIdx, nullptr);
        }

        m_Scenes.Add(newScene);
    }
    m_CurrentSceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
    IE_Assert(m_CurrentSceneIdx < model.scenes.size());

    for (const tinygltf::Camera& camera : model.cameras)
    {
        m_Cameras.Add(camera);
    }
    if (!model.cameras.empty())
    {
        m_CurrentCameraIdx = 0;
        const tinygltf::Camera& defaultCamera = model.cameras[m_CurrentCameraIdx];
        if (defaultCamera.type == "perspective")
        {
            Camera::GetInstance().ConfigurePerspective(static_cast<f32>(defaultCamera.perspective.aspectRatio), static_cast<f32>(defaultCamera.perspective.yfov),
                                                       static_cast<f32>(defaultCamera.perspective.znear), static_cast<f32>(defaultCamera.perspective.zfar));
        }
        else
        {
            Camera::GetInstance().ConfigureOrthographic(static_cast<f32>(defaultCamera.orthographic.xmag), static_cast<f32>(defaultCamera.orthographic.ymag),
                                                        static_cast<f32>(defaultCamera.orthographic.znear), static_cast<f32>(defaultCamera.orthographic.zfar));
        }
    }
}

Vector<SharedPtr<Primitive>> World::GetPrimitives()
{
    Vector<SharedPtr<Primitive>> primitives;
    for (SharedPtr<Node>& rootNode : m_Scenes[m_CurrentSceneIdx].m_RootNodes)
    {
        GetPrimitiveRecursion(primitives, rootNode);
    }
    return primitives;
}

void World::InitNodes(const tinygltf::Model& model, Scene& scene, i32 nodeIndex, const SharedPtr<Node>& parentNode)
{
    const tinygltf::Node& node = model.nodes[nodeIndex];

    float4x4 nodeTransform;
    if (!node.matrix.empty())
    {
        nodeTransform = float4x4(float4(static_cast<f32>(node.matrix[0]), static_cast<f32>(node.matrix[1]), static_cast<f32>(node.matrix[2]), static_cast<f32>(node.matrix[3])),
                                 float4(static_cast<f32>(node.matrix[4]), static_cast<f32>(node.matrix[5]), static_cast<f32>(node.matrix[6]), static_cast<f32>(node.matrix[7])),
                                 float4(static_cast<f32>(node.matrix[8]), static_cast<f32>(node.matrix[9]), static_cast<f32>(node.matrix[10]), static_cast<f32>(node.matrix[11])),
                                 float4(static_cast<f32>(node.matrix[12]), static_cast<f32>(node.matrix[13]), static_cast<f32>(node.matrix[14]), static_cast<f32>(node.matrix[15])));
    }
    else
    {
        float4x4 translation = float4x4::Identity();
        if (!node.translation.empty())
        {
            translation[3][0] = static_cast<f32>(node.translation[0]);
            translation[3][1] = static_cast<f32>(node.translation[1]);
            translation[3][2] = static_cast<f32>(node.translation[2]);
        }

        float4x4 rotation = float4x4::Identity();
        if (!node.rotation.empty())
        {
            const f32 x = static_cast<f32>(-node.rotation[0]);
            const f32 y = static_cast<f32>(-node.rotation[1]);
            const f32 z = static_cast<f32>(-node.rotation[2]);
            const f32 w = static_cast<f32>(node.rotation[3]);

            rotation[0] = {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z), 2.0f * (x * z + w * y), 0.0f};
            rotation[1] = {2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - w * x), 0.0f};
            rotation[2] = {2.0f * (x * z - w * y), 2.0f * (y * z + w * x), 1.0f - 2.0f * (x * x + y * y), 0.0f};
        }

        float4x4 scale = float4x4::Identity();
        if (!node.scale.empty())
        {
            scale[0][0] = static_cast<f32>(node.scale[0]);
            scale[1][1] = static_cast<f32>(node.scale[1]);
            scale[2][2] = static_cast<f32>(node.scale[2]);
        }

        nodeTransform = translation * rotation * scale;
    }

    const SharedPtr<Node> newNode = IE_MakeSharedPtr<Node>();
    newNode->parentNode = parentNode;
    newNode->localTransform = nodeTransform;

    // Check for a mesh attached to the node
    if (node.mesh >= 0)
    {
        const tinygltf::Mesh& mesh = model.meshes[node.mesh];

        SharedPtr<Mesh> newMesh = IE_MakeSharedPtr<Mesh>(newNode);
        newMesh->SetPrimitives(mesh);
        newNode->mesh = newMesh;
    }

    if (!parentNode)
    {
        scene.m_RootNodes.Add(newNode);
    }
    else
    {
        parentNode->nodes.Add(newNode);
    }

    // Traverse child nodes
    for (const i32 childIndex : node.children)
    {
        InitNodes(model, scene, childIndex, newNode);
    }
}