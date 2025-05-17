// Iškur Engine
// Copyright (c) 2025 Tristan Marrec
// Licensed under the MIT License.
// See the LICENSE file in the project root for license information.

#pragma once
#include <dxcapi.h>

#include "Shader.h"

IDxcBlob* CompileShader(ShaderType type, const WString& filename, const Vector<WString>& extraArguments = {});