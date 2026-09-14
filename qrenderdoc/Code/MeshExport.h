/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once
#include "Code/MeshData.h"
#ifdef RENDERDOC_FBX_SDK
enum class FBXAttributeRole
{
  Position,
  Normal,
  UV,
  Tangent,
  Binormal,
  Color,
  Custom
};

struct FBXAttributeMapping
{
  FBXAttributeMapping(FBXAttributeRole r = FBXAttributeRole::Custom, int s = -1,
                      QString n = QString(), int first = 0)
      : role(r), source(s), name(n), firstComponent(first)
  {
  }
  FBXAttributeRole role;
  int source;
  QString name;
  int firstComponent;
  bool remapXYZ = false;
  bool normalizeXYZ = false;
  bool remapTangentW = false;
  bool flipV = false;
  int handednessSource = -1;    // -1 keeps the tangent source fourth component.
  int handednessComponent = 3;
};

QString WriteFBXMesh(const BufferConfiguration &, Topology, const QVector<FBXAttributeMapping> &,
                     const QString &);
bool EditFBXMapping(QWidget *, const BufferConfiguration &, QVector<FBXAttributeMapping> &);
#endif
void ExportMeshesBatch(ICaptureContext &ctx, QWidget *parent, const QVector<uint32_t> &events);

void ConfigureMeshOutputColumns(const PipeState &, int32_t, const ShaderReflection *,
                                rdcarray<ShaderConstant> &, rdcarray<BufferElementProperties> &);
