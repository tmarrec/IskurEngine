// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once

#include "Node.h"

class World;

class Scene
{
    friend World;
    Vector<SharedPtr<Node>> m_RootNodes;
};