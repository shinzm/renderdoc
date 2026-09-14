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
#include <QAtomicInteger>
#include "Code/QRDUtils.h"
struct BufferData
{
  BufferData()
  {
    refcount.store(1);
    stride = 0;
  }

  void ref() { refcount.ref(); }
  void deref()
  {
    bool alive = refcount.deref();

    if(!alive)
      delete this;
  }

  size_t stride;
  bytebuf storage;
  QAtomicInteger<uint32_t> refcount;

  const byte *data() const { return storage.begin(); };
  const byte *end() const { return storage.end(); }
  bool hasData() const { return !storage.empty(); }
  size_t size() const { return storage.size(); }
};

struct BufferElementProperties
{
  ResourceFormat format;
  int buffer = 0;
  ShaderBuiltin systemValue = ShaderBuiltin::Undefined;
  bool perinstance = false;
  bool perprimitive = false;
  bool floatCastWrong = false;
  int instancerate = 1;
};

struct BufferConfiguration
{
  uint32_t curInstance = 0, curView = 0;
  uint32_t numRows = 0, unclampedNumRows = 0;
  uint32_t pagingOffset = 0;

  PackingRules packing;
  ShaderConstant fixedVars;
  rdcarray<ShaderVariable> evalVars;
  uint32_t repeatStride = 1;
  uint32_t repeatOffset = 0;

  QString statusString;

  bool noVertices = false;
  bool noInstances = false;

  // we can have two index buffers for VSOut data:
  // the original index buffer is used for the displayed value (in displayIndices), and the actual
  // potentially remapped or permuated index buffer used for fetching data (in indices).
  BufferData *displayIndices = NULL;
  int32_t displayBaseVertex = 0;
  BufferData *indices = NULL;
  int32_t baseVertex = 0;

  rdcfixedarray<uint32_t, 3> dispatchSize;
  rdcarray<TaskGroupSize> taskSizes;
  rdcarray<uint32_t> meshletVertexPrefixCounts;
  uint32_t taskOrMeshletOffset = 0;
  uint64_t perPrimitiveOffset = 0;
  uint32_t perPrimitiveStride = 0;
  Topology topology = Topology::TriangleList;

  rdcarray<ShaderConstant> columns;
  rdcarray<BufferElementProperties> props;

  QVector<PixelValue> generics;
  QVector<bool> genericsEnabled;
  QList<BufferData *> buffers;
  uint32_t primRestart = 0;

  BufferConfiguration() = default;
  BufferConfiguration(const BufferConfiguration &o) = delete;
  ~BufferConfiguration() { reset(); }
  BufferConfiguration &operator=(const BufferConfiguration &o)
  {
    reset();

    curInstance = o.curInstance;
    numRows = o.numRows;
    unclampedNumRows = o.unclampedNumRows;
    pagingOffset = o.pagingOffset;

    packing = o.packing;
    fixedVars = o.fixedVars;
    evalVars = o.evalVars;
    repeatStride = o.repeatStride;
    repeatOffset = o.repeatOffset;

    statusString = o.statusString;

    noVertices = o.noVertices;
    noInstances = o.noInstances;

    displayIndices = o.displayIndices;
    if(displayIndices)
      displayIndices->ref();
    displayBaseVertex = o.displayBaseVertex;

    indices = o.indices;
    if(indices)
      indices->ref();

    baseVertex = o.baseVertex;
    meshletVertexPrefixCounts = o.meshletVertexPrefixCounts;
    dispatchSize = o.dispatchSize;
    taskSizes = o.taskSizes;
    taskOrMeshletOffset = o.taskOrMeshletOffset;
    perPrimitiveOffset = o.perPrimitiveOffset;
    perPrimitiveStride = o.perPrimitiveStride;
    topology = o.topology;

    columns = o.columns;
    props = o.props;
    generics = o.generics;
    genericsEnabled = o.genericsEnabled;
    primRestart = o.primRestart;

    buffers = o.buffers;
    for(BufferData *b : buffers)
      b->ref();

    return *this;
  }

  void reset()
  {
    if(indices)
      indices->deref();
    indices = NULL;

    if(displayIndices)
      displayIndices->deref();
    displayIndices = NULL;

    for(BufferData *b : buffers)
      b->deref();

    meshletVertexPrefixCounts.clear();
    dispatchSize = {};
    taskSizes.clear();

    buffers.clear();
    columns.clear();
    props.clear();
    generics.clear();
    genericsEnabled.clear();
    numRows = 0;
    unclampedNumRows = 0;

    statusString.clear();

    noVertices = false;
    noInstances = false;
  }

  QString columnName(int col) const
  {
    if(col >= 0 && col < columns.count())
      return columns[col].name;

    return QString();
  }

  int guessPositionColumn() const
  {
    int posEl = -1;

    if(!columns.empty())
    {
      // prioritise system value over general "POSITION" string matching
      for(int i = 0; i < columns.count(); i++)
      {
        const BufferElementProperties &prop = props[i];

        if(prop.systemValue == ShaderBuiltin::Position)
        {
          posEl = i;
          break;
        }
      }

      // look for an exact match
      for(int i = 0; posEl == -1 && i < columns.count(); i++)
      {
        const ShaderConstant &el = columns[i];

        if(QString(el.name).compare(lit("POSITION"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("POSITION0"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("POS"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("POS0"), Qt::CaseInsensitive) == 0)
        {
          posEl = i;
          break;
        }
      }

      // try anything containing position
      for(int i = 0; posEl == -1 && i < columns.count(); i++)
      {
        const ShaderConstant &el = columns[i];

        if(QString(el.name).contains(lit("POSITION"), Qt::CaseInsensitive))
        {
          posEl = i;
          break;
        }
      }

      // OK last resort, just look for 'pos'
      for(int i = 0; posEl == -1 && i < columns.count(); i++)
      {
        const ShaderConstant &el = columns[i];

        if(QString(el.name).contains(lit("POS"), Qt::CaseInsensitive))
        {
          posEl = i;
          break;
        }
      }

      // if we still have absolutely nothing, just use the first available element
      if(posEl == -1)
      {
        posEl = 0;
      }
    }

    return posEl;
  }

  int guessSecondaryColumn() const
  {
    int secondEl = -1;

    if(!columns.empty())
    {
      // prioritise TEXCOORD over general COLOR
      for(int i = 0; i < columns.count(); i++)
      {
        const ShaderConstant &el = columns[i];

        if(QString(el.name).compare(lit("TEXCOORD"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("TEXCOORD0"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("TEX"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("TEX0"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("UV"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("UV0"), Qt::CaseInsensitive) == 0)
        {
          secondEl = i;
          break;
        }
      }

      for(int i = 0; secondEl == -1 && i < columns.count(); i++)
      {
        const ShaderConstant &el = columns[i];

        if(QString(el.name).compare(lit("COLOR"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("COLOR0"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("COL"), Qt::CaseInsensitive) == 0 ||
           QString(el.name).compare(lit("COL0"), Qt::CaseInsensitive) == 0)
        {
          secondEl = i;
          break;
        }
      }
    }

    return secondEl;
  }
};
