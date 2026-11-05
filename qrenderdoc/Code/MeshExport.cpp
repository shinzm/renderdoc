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

#include "MeshExport.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtMath>
#ifdef RENDERDOC_FBX_SDK
#include <fbxsdk.h>
#endif
#ifdef RENDERDOC_FBX_SDK
// Keep wheel input over embedded editors on the table's scroll path. Opening a combo
// popup still allows scrolling its list, but hovering/focusing an editor cannot change it.
class FBXMappingWheelFilter : public QObject
{
public:
  explicit FBXMappingWheelFilter(QTableWidget *table) : QObject(table), m_Table(table) {}

protected:
  bool eventFilter(QObject *watched, QEvent *event) override
  {
    if(event->type() == QEvent::Wheel)
    {
      QApplication::sendEvent(m_Table->viewport(), event);
      return true;
    }
    return QObject::eventFilter(watched, event);
  }

private:
  QTableWidget *m_Table;
};

static QString TransformFBXValues(const FBXAttributeMapping &mapping, QVector<double> &v)
{
  if(mapping.remapXYZ)
    for(int c = 0; c < qMin(3, v.size()); c++)
      v[c] = v[c] * 2.0 - 1.0;
  if(mapping.normalizeXYZ)
  {
    if(v.size() < 3)
      return QObject::tr("XYZ normalization requires three components.");
    double scale = qMax(qAbs(v[0]), qMax(qAbs(v[1]), qAbs(v[2])));
    if(scale == 0.0 || !qIsFinite(scale))
      return QObject::tr("Cannot normalize a zero or non-finite direction.");
    double x = v[0] / scale, y = v[1] / scale, z = v[2] / scale;
    double length = qSqrt(x * x + y * y + z * z);
    v[0] = x / length;
    v[1] = y / length;
    v[2] = z / length;
  }
  if(mapping.remapTangentW)
  {
    if(mapping.role != FBXAttributeRole::Tangent || v.size() != 4)
      return QObject::tr("Tangent W conversion requires a four-component tangent.");
    v[3] = v[3] * 2.0 - 1.0;
  }
  if(mapping.flipV)
  {
    if(mapping.role != FBXAttributeRole::UV || v.size() != 2)
      return QObject::tr("Flip V requires a two-component UV mapping.");
    v[1] = 1.0 - v[1];
  }
  for(double value : v)
    if(!qIsFinite(value))
      return QObject::tr("Attribute conversion produced a non-finite value.");
  return QString();
}

static int FBXComponentCount(const FBXAttributeMapping &mapping, const ShaderConstant &el)
{
  int available = int(el.type.rows * el.type.columns) - mapping.firstComponent;
  if(mapping.role == FBXAttributeRole::Custom)
    return available;
  if(mapping.role == FBXAttributeRole::UV)
    return 2;
  if(mapping.role == FBXAttributeRole::Color || mapping.role == FBXAttributeRole::Tangent)
    return qMin(available, 4);
  return 3;
}

static QString ValidateHandedness(const FBXAttributeMapping &m, const BufferConfiguration &config)
{
  if(m.handednessSource < 0)
    return QString();
  int source = m.handednessSource;
  if(m.role != FBXAttributeRole::Tangent || source >= config.columns.count() ||
     source >= config.props.count())
    return QObject::tr("Invalid tangent handedness source.");
  const ShaderConstant &el = config.columns[source];
  if(m.handednessComponent < 0 || m.handednessComponent >= int(el.type.rows * el.type.columns) ||
     config.props[source].perprimitive ||
     (source < config.genericsEnabled.size() && config.genericsEnabled[source]))
    return QObject::tr("Handedness source has an unavailable component or unsupported data rate.");
  return QString();
}

QString WriteFBXMesh(const BufferConfiguration &config, Topology topology,
                     const QVector<FBXAttributeMapping> &selected, const QString &filename)
{
  QString error;
  int positionCount = 0;
  QSet<QString> names;
  for(const FBXAttributeMapping &mapping : selected)
  {
    if(mapping.source < 0)
      continue;
    if(mapping.source >= config.columns.count() || mapping.source >= config.props.count())
      return QObject::tr("Invalid attribute source.");
    error = ValidateHandedness(mapping, config);
    if(!error.isEmpty())
      return error;
    const ShaderConstant &el = config.columns[mapping.source];
    int minimum = mapping.role == FBXAttributeRole::Custom ? 1
                  : mapping.role == FBXAttributeRole::UV   ? 2
                                                           : 3;
    if(mapping.firstComponent < 0 ||
       int(el.type.rows * el.type.columns) - mapping.firstComponent < minimum ||
       config.props[mapping.source].perprimitive ||
       (mapping.source < config.genericsEnabled.size() && config.genericsEnabled[mapping.source]))
      return QObject::tr("Attribute has insufficient components or unsupported data rate.");
    if(mapping.name.trimmed().isEmpty() || names.contains(mapping.name))
      return QObject::tr("Every exported attribute needs a unique, non-empty name.");
    names.insert(mapping.name);
    positionCount += mapping.role == FBXAttributeRole::Position ? 1 : 0;
  }
  if(positionCount != 1)
    return QObject::tr("Select exactly one position attribute.");
  FbxManager *manager = FbxManager::Create();
  if(!manager)
  {
    error = QObject::tr("Could not create FBX manager.");
    return error;
  }
  FbxScene *scene = FbxScene::Create(manager, "RenderDoc");
  FbxMesh *mesh = FbxMesh::Create(scene, "Mesh");
  FbxNode *node = FbxNode::Create(scene, "Mesh");
  node->SetNodeAttribute(mesh);
  scene->GetRootNode()->AddChild(node);
  QVector<FbxLayerElement *> layers(selected.size(), NULL);
  QVector<FbxLayerElementUserData *> custom(selected.size(), NULL);
  for(int i = 0; i < selected.size(); i++)
  {
    const FBXAttributeMapping &mapping = selected[i];
    if(mapping.source < 0 || mapping.role == FBXAttributeRole::Position)
      continue;
    const QByteArray name = mapping.name.toUtf8();
    switch(mapping.role)
    {
      case FBXAttributeRole::Normal: layers[i] = mesh->CreateElementNormal(); break;
      case FBXAttributeRole::Tangent: layers[i] = mesh->CreateElementTangent(); break;
      case FBXAttributeRole::Binormal: layers[i] = mesh->CreateElementBinormal(); break;
      case FBXAttributeRole::UV:
      {
        // Fill existing layers before creating one: holes in the UV layer sequence cause
        // the SDK writer to synthesize unnamed UV sets when colors/user data share layers.
        FbxGeometryElementUV *uv = FbxGeometryElementUV::Create(mesh, name.constData());
        int layer = 0;
        while(layer < mesh->GetLayerCount() && mesh->GetLayer(layer)->GetUVs())
          layer++;
        if(layer == mesh->GetLayerCount())
          mesh->CreateLayer();
        mesh->GetLayer(layer)->SetUVs(uv);
        layers[i] = uv;
        break;
      }
      case FBXAttributeRole::Color: layers[i] = mesh->CreateElementVertexColor(); break;
      default: break;
    }
    if(layers[i])
    {
      layers[i]->SetName(name.constData());
      layers[i]->SetMappingMode(FbxLayerElement::eByControlPoint);
      layers[i]->SetReferenceMode(FbxLayerElement::eDirect);
    }
    // FBX tangent layer serialization only retains XYZ. Preserve handedness separately.
    int components = FBXComponentCount(mapping, config.columns[mapping.source]);
    if(mapping.role == FBXAttributeRole::Custom ||
       (mapping.role == FBXAttributeRole::Tangent &&
        (components == 4 || mapping.handednessSource >= 0)))
    {
      int count = mapping.role == FBXAttributeRole::Custom ? components : 1;
      FbxArray<FbxDataType> types;
      FbxArray<const char *> fieldNames;
      QVector<QByteArray> storage;
      storage.reserve(count);
      for(int c = 0; c < count; c++)
        storage.push_back((mapping.name + lit(".") +
                           (mapping.role == FBXAttributeRole::Tangent ? lit("w") : QString::number(c)))
                              .toUtf8());
      for(int c = 0; c < count; c++)
      {
        types.Add(FbxDoubleDT);
        fieldNames.Add(storage[c].constData());
      }
      custom[i] = FbxLayerElementUserData::Create(mesh, name.constData(), i, types, fieldNames);
      custom[i]->SetMappingMode(FbxLayerElement::eByControlPoint);
      custom[i]->SetReferenceMode(FbxLayerElement::eDirect);
      int layer = 0;
      while(layer < mesh->GetLayerCount() && mesh->GetLayer(layer)->GetUserData())
        layer++;
      if(layer == mesh->GetLayerCount())
        mesh->CreateLayer();
      mesh->GetLayer(layer)->SetUserData(custom[i]);
    }
  }
  QVector<FBXAttributeMapping> decodeMappings = selected;
  QVector<int> handednessIndices(selected.size(), -1);
  for(int i = 0; i < selected.size(); i++)
    if(selected[i].source >= 0 && selected[i].handednessSource >= 0)
    {
      handednessIndices[i] = decodeMappings.size();
      decodeMappings.push_back(FBXAttributeMapping(FBXAttributeRole::Custom,
                                                   selected[i].handednessSource, QString(),
                                                   selected[i].handednessComponent));
    }
  QHash<uint32_t, int> vertices;
  QVector<int> primitive;
  uint32_t stripVertex = 0;
  for(uint32_t row = 0; row < config.numRows && error.isEmpty(); row++)
  {
    if(config.indices && uint64_t(row + 1) * sizeof(uint32_t) > config.indices->size())
    {
      error = QObject::tr("Missing index data at row %1.").arg(row);
      break;
    }
    uint32_t rawIndex = row;
    if(config.indices)
      memcpy(&rawIndex, config.indices->data() + uint64_t(row) * sizeof(uint32_t), sizeof(rawIndex));
    if(config.indices && config.primRestart && rawIndex == config.primRestart)
    {
      primitive.clear();
      stripVertex = 0;
      continue;
    }
    const int64_t adjustedIndex = int64_t(rawIndex) + (config.indices ? config.baseVertex : 0);
    if(adjustedIndex < 0 || adjustedIndex > int64_t(UINT32_MAX))
    {
      error = QObject::tr("Invalid vertex index at row %1.").arg(row);
      break;
    }
    const uint32_t idx = uint32_t(adjustedIndex);
    int vertex = vertices.value(idx, -1);
    if(vertex < 0)
    {
      QVector<QVector<double>> values(decodeMappings.size());
      for(int role = 0; role < decodeMappings.size() && error.isEmpty(); role++)
      {
        if(decodeMappings[role].source < 0)
          continue;
        const ShaderConstant &el = config.columns[decodeMappings[role].source];
        const BufferElementProperties &prop = config.props[decodeMappings[role].source];
        if(prop.buffer < 0 || prop.buffer >= config.buffers.size())
        {
          error = QObject::tr("Missing attribute buffer at row %1.").arg(row);
          break;
        }
        const BufferData *buffer = config.buffers[prop.buffer];
        uint32_t element = prop.perinstance
                               ? (prop.instancerate > 0 ? config.curInstance / prop.instancerate : 0)
                               : idx;
        // Validate integer offsets before forming pointers, including instance/attribute offsets.
        uint64_t available = buffer->size();
        if(el.byteOffset > available ||
           (buffer->stride && element > (available - el.byteOffset) / buffer->stride))
        {
          error = QObject::tr("Missing or out-of-range attribute data at row %1.").arg(row);
          break;
        }
        uint64_t offset = el.byteOffset + uint64_t(buffer->stride) * element;
        if(!buffer->hasData() || el.type.arrayByteStride > available - offset)
        {
          error = QObject::tr("Missing or out-of-range attribute data at row %1.").arg(row);
          break;
        }
        const byte *data = buffer->data() + offset;
        QVariantList list = GetVariants(prop.format, el, data, buffer->end());
        const int components =
            role < selected.size() ? FBXComponentCount(decodeMappings[role], el) : 1;
        if(list.size() < components + decodeMappings[role].firstComponent)
        {
          error = QObject::tr("Could not decode attribute at row %1.").arg(row);
          break;
        }
        values[role].resize(components);
        for(int c = 0; c < components; c++)
        {
          bool ok = false;
          values[role][c] = list[c + decodeMappings[role].firstComponent].toDouble(&ok);
          if(!ok || !qIsFinite(values[role][c]))
            error = QObject::tr("Non-numeric or non-finite attribute at row %1.").arg(row);
          const QVariant &component = list[c + decodeMappings[role].firstComponent];
          if((GetVariantMetatype(component) == QMetaType::ULongLong &&
              component.toULongLong() > 9007199254740992ULL) ||
             (GetVariantMetatype(component) == QMetaType::LongLong &&
              (component.toLongLong() > 9007199254740992LL ||
               component.toLongLong() < -9007199254740992LL)))
            error =
                QObject::tr("Integer attribute at row %1 exceeds exact double precision.").arg(row);
        }
      }
      if(!error.isEmpty())
        break;
      for(int i = 0; i < selected.size(); i++)
        if(handednessIndices[i] >= 0)
        {
          values[i].resize(4);
          values[i][3] = values[handednessIndices[i]][0];
        }
      for(int role = 0; role < selected.size() && error.isEmpty(); role++)
        if(selected[role].source >= 0)
          error = TransformFBXValues(selected[role], values[role]);
      if(!error.isEmpty())
        break;
      vertex = vertices.size();
      vertices.insert(idx, vertex);
      for(int i = 0; i < selected.size(); i++)
      {
        if(selected[i].source < 0)
          continue;
        const QVector<double> &v = values[i];
        switch(selected[i].role)
        {
          case FBXAttributeRole::Position:
            mesh->SetControlPointAt(FbxVector4(v[0], v[1], v[2]), vertex);
            break;
          case FBXAttributeRole::Normal:
            static_cast<FbxGeometryElementNormal *>(layers[i])->GetDirectArray().Add(
                FbxVector4(v[0], v[1], v[2]));
            break;
          case FBXAttributeRole::Tangent:
            static_cast<FbxGeometryElementTangent *>(layers[i])->GetDirectArray().Add(
                FbxVector4(v[0], v[1], v[2], v.size() > 3 ? v[3] : 1.0));
            break;
          case FBXAttributeRole::Binormal:
            static_cast<FbxGeometryElementBinormal *>(layers[i])->GetDirectArray().Add(
                FbxVector4(v[0], v[1], v[2]));
            break;
          case FBXAttributeRole::UV:
            static_cast<FbxGeometryElementUV *>(layers[i])->GetDirectArray().Add(
                FbxVector2(v[0], v[1]));
            break;
          case FBXAttributeRole::Color:
            static_cast<FbxGeometryElementVertexColor *>(layers[i])->GetDirectArray().Add(
                FbxColor(v[0], v[1], v[2], v.size() > 3 ? v[3] : 1.0));
            break;
          default: break;
        }
        if(custom[i])
        {
          if(selected[i].role == FBXAttributeRole::Tangent)
            FbxGetDirectArray<double>(custom[i], 0).Add(v[3]);
          else
            for(int c = 0; c < v.size(); c++)
              FbxGetDirectArray<double>(custom[i], c).Add(v[c]);
        }
      }
    }
    primitive.push_back(vertex);
    if(primitive.size() == 3)
    {
      int a = primitive[0], b = primitive[1], c = primitive[2];
      if(topology == Topology::TriangleStrip && (stripVertex & 1))
        qSwap(a, b);
      if(a != b && b != c && a != c)
      {
        mesh->BeginPolygon();
        mesh->AddPolygon(a);
        mesh->AddPolygon(b);
        mesh->AddPolygon(c);
        mesh->EndPolygon();
      }
      if(topology == Topology::TriangleList)
        primitive.clear();
      else
        primitive.remove(topology == Topology::TriangleFan ? 1 : 0);
      stripVertex++;
    }
  }
  if(error.isEmpty() && mesh->GetPolygonCount() == 0)
    error = QObject::tr("No non-degenerate triangles to export.");
  // Write to a temporary file first, then atomically replace the requested destination.
  QTemporaryDir temp;
  QString path = temp.filePath(lit("mesh.fbx"));
  if(error.isEmpty())
  {
    FbxIOSettings *settings = FbxIOSettings::Create(manager, IOSROOT);
    manager->SetIOSettings(settings);
    FbxExporter *exporter = FbxExporter::Create(manager, "");
    if(!temp.isValid() || !exporter->Initialize(path.toUtf8().constData(), -1, settings) ||
       !exporter->Export(scene))
      error = QObject::tr("FBX export failed: %1")
                  .arg(QString::fromUtf8(exporter->GetStatus().GetErrorString()));
    exporter->Destroy();
  }
  manager->Destroy();
  if(error.isEmpty())
  {
    QFile source(path);
    QSaveFile destination(filename);
    if(!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly))
      error = QObject::tr("Could not open the FBX file for copying.");
    while(error.isEmpty() && !source.atEnd())
    {
      QByteArray chunk = source.read(1024 * 1024);
      if(source.error() != QFile::NoError || destination.write(chunk) != chunk.size())
        error = QObject::tr("Could not write the FBX file.");
    }
    if(error.isEmpty() && !destination.commit())
      error = destination.errorString();
  }
  return error;
}

#if ENABLE_UNIT_TESTS
#include "3rdparty/catch/catch.hpp"

TEST_CASE("FBX mesh export round trip", "[fbx]")
{
  BufferConfiguration config;
  const float data[][8] = {
      {0, 0, 0, 0, 0, 1, 0, 0}, {1, 0, 0, 0, 0, 1, 1, 0},       {0, 1, 0, 0, 0, 1, 0, 1},
      {1, 1, 0, 0, 0, 1, 1, 1}, {0, 0, 0, 0, 0, 1, 0.5f, 0.5f},
  };
  BufferData *buffer = new BufferData;
  buffer->stride = sizeof(data[0]);
  buffer->storage.resize(sizeof(data));
  memcpy(buffer->storage.data(), data, sizeof(data));
  config.buffers.push_back(buffer);
  for(int role = 0; role < 3; role++)
  {
    ShaderConstant el;
    el.name = role == 0 ? "position" : role == 1 ? "normal" : "uv";
    el.byteOffset = role * 3 * sizeof(float);
    el.type.rows = 1;
    el.type.columns = role == 2 ? 2 : 3;
    el.type.baseType = VarType::Float;
    el.type.arrayByteStride = el.type.columns * sizeof(float);
    BufferElementProperties prop;
    prop.format.type = ResourceFormatType::Regular;
    prop.format.compType = CompType::Float;
    prop.format.compCount = el.type.columns;
    prop.format.compByteWidth = sizeof(float);
    config.columns.push_back(el);
    config.props.push_back(prop);
  }
  QVector<uint32_t> indices = {0, 1, 2, 2, 1, 3, 4, 1, 2};
  Topology topology = Topology::TriangleList;
  QVector<FBXAttributeMapping> selected = {{FBXAttributeRole::Position, 0, lit("Position")},
                                           {FBXAttributeRole::Normal, 1, lit("Normal")},
                                           {FBXAttributeRole::UV, 2, lit("UV0")}};
  QVector<int> expected = {0, 1, 2, 2, 1, 3, 4, 1, 2};
  int points = 5;
  bool indexed = true;
  bool invalid = false;
  bool packedNormal = false;
  bool convertedNormal = false;
  bool instanceUV = false;
  bool extendedAttributes = false;

  SECTION("independent handedness for a three component tangent")
  {
    FBXAttributeMapping tangent(FBXAttributeRole::Tangent, 1, lit("IndependentTangent"));
    tangent.handednessSource = 2;
    tangent.handednessComponent = 1;
    tangent.remapTangentW = true;
    selected.push_back(tangent);
  }
  SECTION("invalid handedness component preserves destination")
  {
    FBXAttributeMapping tangent(FBXAttributeRole::Tangent, 1, lit("IndependentTangent"));
    tangent.handednessSource = 2;
    tangent.handednessComponent = 3;
    selected.push_back(tangent);
    invalid = true;
  }
  SECTION("indexed list preserves UV seams")
  {
  }
  SECTION("multiple UV color tangent and custom sets round trip")
  {
    extendedAttributes = true;
    BufferData *extra = new BufferData;
    const float attribute[4] = {0.25f, 0.5f, 0.75f, -1.0f};
    extra->stride = sizeof(attribute);
    extra->storage.resize(points * sizeof(attribute));
    for(int i = 0; i < points; i++)
      memcpy(extra->storage.data() + i * sizeof(attribute), attribute, sizeof(attribute));
    config.buffers.push_back(extra);
    ShaderConstant el = config.columns[0];
    el.name = "extra";
    el.type.columns = 4;
    el.type.arrayByteStride = sizeof(attribute);
    config.columns.push_back(el);
    BufferElementProperties prop = config.props[0];
    prop.buffer = 1;
    prop.format.compCount = 4;
    config.props.push_back(prop);
    selected.push_back({FBXAttributeRole::Tangent, 3, lit("Tangent0")});
    selected.push_back({FBXAttributeRole::Binormal, 1, lit("Binormal0")});
    selected.push_back({FBXAttributeRole::Color, 3, lit("Color0")});
    selected.push_back({FBXAttributeRole::Color, 1, lit("Color1")});
    selected.push_back({FBXAttributeRole::Custom, 3, lit("Weights")});
    selected.push_back({FBXAttributeRole::Custom, 2, lit("ExtraData")});
    for(int i = 1; i < 10; i++)
      selected.push_back({FBXAttributeRole::UV, 3, lit("UV") + QString::number(i), 2});
  }
  SECTION("duplicate output names preserve destination")
  {
    selected[2].name = selected[1].name;
    invalid = true;
  }
  SECTION("invalid UV component offset preserves destination")
  {
    selected[2].firstComponent = 1;
    invalid = true;
  }
  SECTION("strip restarts and alternates winding")
  {
    topology = Topology::TriangleStrip;
    config.primRestart = ~0U;
    indices = {0, 1, 2, 3, ~0U, 4, 1, 2};
  }
  SECTION("fan")
  {
    topology = Topology::TriangleFan;
    indices = {0, 1, 3, 2};
    expected = {0, 1, 2, 0, 2, 3};
    points = 4;
  }
  SECTION("negative base vertex")
  {
    config.baseVertex = -5;
    for(uint32_t &index : indices)
      index += 5;
  }
  SECTION("non-indexed positions only")
  {
    indexed = false;
    points = 3;
    expected = {0, 1, 2};
    selected[1].source = selected[2].source = -1;
  }
  SECTION("remapped and normalized normals")
  {
    convertedNormal = true;
    selected[1].remapXYZ = true;
    selected[1].normalizeXYZ = true;
  }
  SECTION("flipped UV survives FBX round trip")
  {
    selected[2].flipV = true;
  }
  SECTION("packed normalized normals")
  {
    packedNormal = true;
    BufferData *normalBuffer = new BufferData;
    normalBuffer->stride = sizeof(uint32_t);
    normalBuffer->storage.resize(points * sizeof(uint32_t));
    const uint32_t normal = 1023U << 20;
    for(int i = 0; i < points; i++)
      memcpy(normalBuffer->storage.data() + i * sizeof(uint32_t), &normal, sizeof(normal));
    config.buffers.push_back(normalBuffer);
    config.props[1].buffer = 1;
    config.props[1].format.type = ResourceFormatType::R10G10B10A2;
    config.props[1].format.compType = CompType::UNorm;
    config.props[1].format.compCount = 4;
    config.columns[1].byteOffset = 0;
    config.columns[1].type.arrayByteStride = sizeof(uint32_t);
  }
  SECTION("instance UV")
  {
    instanceUV = true;
    config.curInstance = 3;
    config.props[2].perinstance = true;
    config.props[2].instancerate = 2;
  }
  SECTION("truncated attributes preserve destination")
  {
    buffer->storage.resize(sizeof(data[0]));
    invalid = true;
  }
  SECTION("attribute offset beyond buffer preserves destination")
  {
    config.columns[0].byteOffset = 10000;
    invalid = true;
  }
  SECTION("missing vertex buffer preserves destination")
  {
    config.props[0].buffer = 10;
    invalid = true;
  }
  SECTION("instance offset beyond buffer preserves destination")
  {
    config.curInstance = 100;
    config.props[2].perinstance = true;
    invalid = true;
  }
  SECTION("out of range indices preserve destination")
  {
    indices[0] = 100;
    invalid = true;
  }
  SECTION("base vertex underflow preserves destination")
  {
    config.baseVertex = -1;
    invalid = true;
  }
  SECTION("non finite positions preserve destination")
  {
    float value = qQNaN();
    memcpy(buffer->storage.data(), &value, sizeof(value));
    invalid = true;
  }

  config.numRows = indexed ? indices.size() : 3;
  if(indexed)
  {
    config.indices = new BufferData;
    config.indices->storage.resize(indices.size() * sizeof(uint32_t));
    memcpy(config.indices->storage.data(), indices.data(), config.indices->size());
  }
  QTemporaryDir temp;
  REQUIRE(temp.isValid());
  QString filename = temp.filePath(QString::fromUtf8("mesh-测试.fbx"));
  if(invalid)
  {
    QFile original(filename);
    REQUIRE(original.open(QIODevice::WriteOnly));
    original.write("keep");
    original.close();
    CHECK_FALSE(WriteFBXMesh(config, topology, selected, filename).isEmpty());
    REQUIRE(original.open(QIODevice::ReadOnly));
    CHECK(original.readAll() == QByteArray("keep"));
    return;
  }
  const QString error = WriteFBXMesh(config, topology, selected, filename);
  INFO(error.toStdString());
  REQUIRE(error.isEmpty());
  struct ManagerOwner
  {
    FbxManager *manager = FbxManager::Create();
    ~ManagerOwner() { manager->Destroy(); }
  } owner;
  FbxScene *scene = FbxScene::Create(owner.manager, "RoundTrip");
  FbxImporter *importer = FbxImporter::Create(owner.manager, "");
  REQUIRE(importer->Initialize(filename.toUtf8().constData()));
  REQUIRE(importer->Import(scene));
  REQUIRE(scene->GetRootNode()->GetChildCount() == 1);
  FbxMesh *mesh = scene->GetRootNode()->GetChild(0)->GetMesh();
  REQUIRE(mesh != NULL);
  REQUIRE(mesh->GetControlPointsCount() == points);
  if(extendedAttributes)
  {
    QString uvNames;
    for(int i = 0; i < mesh->GetElementUVCount(); i++)
      uvNames += QString::fromUtf8(mesh->GetElementUV(i)->GetName()) + lit(", ");
    INFO(uvNames.toStdString());
    REQUIRE(mesh->GetElementUVCount() == 10);
    REQUIRE(mesh->GetElementVertexColorCount() == 2);
    REQUIRE(mesh->GetElementTangentCount() == 1);
    REQUIRE(mesh->GetElementBinormalCount() == 1);
    REQUIRE(mesh->GetElementUserDataCount() == 3);
    for(int set = 1; set < 10; set++)
    {
      CHECK((QString::fromUtf8(mesh->GetElementUV(set)->GetName()) ==
             lit("UV") + QString::number(set)));
      REQUIRE(mesh->GetElementUV(set)->GetDirectArray().GetCount() == points);
      for(int i = 0; i < points; i++)
      {
        CHECK(mesh->GetElementUV(set)->GetDirectArray().GetAt(i)[0] == 0.75);
        CHECK(mesh->GetElementUV(set)->GetDirectArray().GetAt(i)[1] == -1.0);
      }
    }
    for(int i = 0; i < points; i++)
    {
      for(int c = 0; c < 3; c++)
      {
        CHECK(mesh->GetElementTangent()->GetDirectArray().GetAt(i)[c] == (c + 1) * 0.25);
        CHECK(mesh->GetElementBinormal()->GetDirectArray().GetAt(i)[c] == (c == 2 ? 1.0 : 0.0));
      }
      FbxColor color = mesh->GetElementVertexColor(0)->GetDirectArray().GetAt(i);
      CHECK(color.mRed == 0.25);
      CHECK(color.mGreen == 0.5);
      CHECK(color.mBlue == 0.75);
      // FbxColor clamps channels to [0, 1]; the custom Weights set preserves the original -1.
      CHECK(color.mAlpha == 0.0);
      CHECK(mesh->GetElementVertexColor(1)->GetDirectArray().GetAt(i).mAlpha == 1.0);
    }
    for(int set = 0; set < mesh->GetElementUserDataCount(); set++)
    {
      FbxLayerElementUserData *user = mesh->GetElementUserData(set);
      QString name = QString::fromUtf8(user->GetName());
      REQUIRE((name == lit("Tangent0") || name == lit("Weights") || name == lit("ExtraData")));
      int count = name == lit("Tangent0") ? 1 : name == lit("Weights") ? 4 : 2;
      REQUIRE(user->GetDirectArrayCount() == count);
      for(int c = 0; c < count; c++)
      {
        REQUIRE(FbxGetDirectArray<double>(user, c).GetCount() == points);
        for(int i = 0; i < points; i++)
        {
          double expectedValue = name == lit("ExtraData")            ? data[i][6 + c]
                                 : name == lit("Tangent0") || c == 3 ? -1.0
                                                                     : (c + 1) * 0.25;
          CHECK(FbxGetDirectArray<double>(user, c).GetAt(i) == expectedValue);
        }
      }
    }
  }
  REQUIRE(mesh->GetPolygonCount() == expected.size() / 3);
  for(int i = 0; i < expected.size(); i++)
    CHECK(mesh->GetPolygonVertex(i / 3, i % 3) == expected[i]);
  for(int i = 0; i < points; i++)
  {
    int source = topology == Topology::TriangleFan ? int(indices[i]) : i;
    for(int c = 0; c < 3; c++)
      CHECK(mesh->GetControlPointAt(i)[c] == double(data[source][c]));
    if(selected[1].source >= 0)
    {
      REQUIRE(mesh->GetElementNormal() != NULL);
      for(int c = 0; c < 3; c++)
        CHECK(mesh->GetElementNormal()->GetDirectArray().GetAt(i)[c] ==
              (convertedNormal ? (c == 2 ? 1.0 : -1.0) / qSqrt(3.0)
                               : double(packedNormal ? (c == 2 ? 1 : 0) : data[source][3 + c])));
    }
    if(selected[2].source >= 0)
    {
      if(selected.last().name == lit("IndependentTangent"))
      {
        REQUIRE(mesh->GetElementUserDataCount() == 1);
        CHECK(FbxGetDirectArray<double>(mesh->GetElementUserData(0), 0).GetAt(i) ==
              double(data[source][7]) * 2.0 - 1.0);
        for(int c = 0; c < 3; c++)
          CHECK(mesh->GetElementTangent()->GetDirectArray().GetAt(i)[c] ==
                double(data[source][3 + c]));
      }
      REQUIRE(mesh->GetElementUV() != NULL);
      for(int c = 0; c < 2; c++)
        CHECK(mesh->GetElementUV()->GetDirectArray().GetAt(i)[c] ==
              (selected[2].flipV && c == 1 ? 1.0 - double(data[instanceUV ? 1 : source][6 + c])
                                           : double(data[instanceUV ? 1 : source][6 + c])));
    }
  }
}

TEST_CASE("FBX UV flip is optional and preserves tiled coordinates", "[fbx]")
{
  FBXAttributeMapping mapping(FBXAttributeRole::UV, 0, lit("UV0"));
  QVector<double> v = {0.25, -1.0};
  REQUIRE(TransformFBXValues(mapping, v).isEmpty());
  CHECK(v[1] == -1.0);
  mapping.flipV = true;
  REQUIRE(TransformFBXValues(mapping, v).isEmpty());
  CHECK(v[0] == 0.25);
  CHECK(v[1] == 2.0);
  mapping.role = FBXAttributeRole::Normal;
  CHECK_FALSE(TransformFBXValues(mapping, v).isEmpty());
}

TEST_CASE("FBX direction conversions keep tangent W independent", "[fbx]")
{
  FBXAttributeMapping mapping(FBXAttributeRole::Tangent, 0, lit("Tangent"));
  QVector<double> v = {0.0, 0.5, 1.0, 0.0};
  REQUIRE(TransformFBXValues(mapping, v).isEmpty());
  CHECK(v[0] == 0.0);
  CHECK(v[1] == 0.5);
  CHECK(v[2] == 1.0);
  CHECK(v[3] == 0.0);
  mapping.remapXYZ = true;
  mapping.normalizeXYZ = true;
  REQUIRE(TransformFBXValues(mapping, v).isEmpty());
  CHECK(v[0] == -1.0 / qSqrt(2.0));
  CHECK(v[1] == 0.0);
  CHECK(v[2] == 1.0 / qSqrt(2.0));
  CHECK(v[3] == 0.0);
  mapping.remapXYZ = mapping.normalizeXYZ = false;
  mapping.remapTangentW = true;
  REQUIRE(TransformFBXValues(mapping, v).isEmpty());
  CHECK(v[3] == -1.0);
  v[3] = 1.0;
  REQUIRE(TransformFBXValues(mapping, v).isEmpty());
  CHECK(v[3] == 1.0);
  mapping.remapTangentW = false;
  mapping.normalizeXYZ = true;
  v = {0.0, 0.0, 0.0, -1.0};
  CHECK_FALSE(TransformFBXValues(mapping, v).isEmpty());
}
#endif
#endif

#ifdef RENDERDOC_FBX_SDK
bool EditFBXMapping(QWidget *parent, const BufferConfiguration &config,
                    QVector<FBXAttributeMapping> &selected)
{
  QDialog dialog(parent);
  dialog.setWindowTitle(QObject::tr("FBX attribute mapping"));
  QFormLayout *layout = new QFormLayout(&dialog);
  QLabel *description = new QLabel(
      QObject::tr(
          "Export the selected stage and instance. Choose source attributes below.\n"
          "XYZ is copied as-is: no perspective divide, axis or unit conversion.\n"
          "Shader output positions may be in clip space. UVs flip only with Flip V enabled."),
      &dialog);
  description->setWordWrap(true);
  layout->addRow(description);
  QLabel *details = new QLabel(
      QObject::tr(
          "Add UV, tangent, binormal, color or custom sets as needed. First component: 0 = X, 2 = "
          "Z.\n"
          "Custom data includes bone indices/weights, but does not reconstruct skinning.\n"
          "Tangent W is also stored as custom data. Colors clamp to [0, 1]; RGB uses alpha 1."),
      &dialog);
  layout->addRow(details);
  QTableWidget *table = new QTableWidget(0, 10, &dialog);
  table->setHorizontalHeaderLabels(
      {QObject::tr("Usage"), QObject::tr("Source attribute"), QObject::tr("First component"),
       QObject::tr("Export name"), QObject::tr("XYZ remap"), QObject::tr("Normalize XYZ"),
       QObject::tr("Tangent W remap"), QObject::tr("Flip V"), QObject::tr("Handedness source"),
       QObject::tr("Handedness component")});
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  // Size rows from the actual editors (including DPI/style minimum sizes), and scroll
  // their geometry in pixels together with the painted item cells.
  table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  FBXMappingWheelFilter *wheelFilter = new FBXMappingWheelFilter(table);
  layout->addRow(table);
  QLabel *validation = new QLabel(&dialog);
  validation->setWordWrap(true);
  layout->addRow(validation);
  QDialogButtonBox *buttons =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  const QStringList usages = {QObject::tr("Position XYZ"),
                              QObject::tr("Normal XYZ"),
                              QObject::tr("UV XY"),
                              QObject::tr("Tangent XYZ(W)"),
                              QObject::tr("Binormal XYZ"),
                              QObject::tr("Color RGB(A)"),
                              QObject::tr("Custom / weights / bone indices")};
  auto readMapping = [=]() {
    QVector<FBXAttributeMapping> result;
    for(int row = 0; row < table->rowCount(); row++)
    {
      result.push_back(FBXAttributeMapping(
          FBXAttributeRole(static_cast<QComboBox *>(table->cellWidget(row, 0))->currentIndex()),
          static_cast<QComboBox *>(table->cellWidget(row, 1))->currentData().toInt(),
          table->item(row, 3)->text().trimmed(),
          static_cast<QSpinBox *>(table->cellWidget(row, 2))->value()));
      result.last().remapXYZ = static_cast<QCheckBox *>(table->cellWidget(row, 4))->isChecked();
      result.last().normalizeXYZ = static_cast<QCheckBox *>(table->cellWidget(row, 5))->isChecked();
      result.last().remapTangentW = static_cast<QCheckBox *>(table->cellWidget(row, 6))->isChecked();
      result.last().handednessSource =
          static_cast<QComboBox *>(table->cellWidget(row, 8))->currentData().toInt();
      result.last().handednessComponent =
          static_cast<QComboBox *>(table->cellWidget(row, 9))->currentIndex();
      result.last().flipV = static_cast<QCheckBox *>(table->cellWidget(row, 7))->isChecked();
    }
    return result;
  };
  auto updateSave = [=, &config]() {
    int positions = 0;
    QString error;
    QSet<QString> names;
    for(const FBXAttributeMapping &m : readMapping())
    {
      if(m.source < 0)
        continue;
      const ShaderConstant &el = config.columns[m.source];
      int minimum = m.role == FBXAttributeRole::Custom ? 1 : m.role == FBXAttributeRole::UV ? 2 : 3;
      if(int(el.type.rows * el.type.columns) - m.firstComponent < minimum)
        error = QObject::tr(
            "A selected source has too few components for its usage and component offset.");
      if(m.name.isEmpty() || names.contains(m.name))
        error = QObject::tr("Export names must be non-empty and unique.");
      if(m.normalizeXYZ && FBXComponentCount(m, el) < 3)
        error = QObject::tr("XYZ normalization requires three exported components.");
      if(m.remapTangentW && (m.role != FBXAttributeRole::Tangent ||
                             (FBXComponentCount(m, el) != 4 && m.handednessSource < 0)))
        error = QObject::tr("W remapping requires a four-component tangent.");
      QString handednessError = ValidateHandedness(m, config);
      if(!handednessError.isEmpty())
        error = handednessError;
      names.insert(m.name);
      positions += m.role == FBXAttributeRole::Position ? 1 : 0;
    }
    if(positions != 1)
      error = QObject::tr("Select exactly one position source. Rows with source None are omitted.");
    validation->setText(error);
    buttons->button(QDialogButtonBox::Save)->setEnabled(error.isEmpty());
  };
  auto addMapping = [=, &config, &dialog](FBXAttributeRole usage, int source, const QString &name) {
    int row = table->rowCount();
    table->insertRow(row);
    QComboBox *role = new QComboBox(table);
    role->addItems(usages);
    role->setCurrentIndex(int(usage));
    QComboBox *attribute = new QComboBox(table);
    attribute->addItem(QObject::tr("None"), -1);
    for(int col = 0; col < config.columns.count(); col++)
    {
      const ShaderConstant &el = config.columns[col];
      if(el.type.rows * el.type.columns == 0 || config.props[col].perprimitive ||
         (col < config.genericsEnabled.size() && config.genericsEnabled[col]))
        continue;
      attribute->addItem(
          QObject::tr("%1 (%2 components)").arg(QString(el.name)).arg(el.type.rows * el.type.columns),
          col);
    }
    attribute->setCurrentIndex(qMax(0, attribute->findData(source)));
    QSpinBox *first = new QSpinBox(table);
    first->setRange(0, 255);
    role->installEventFilter(wheelFilter);
    attribute->installEventFilter(wheelFilter);
    first->installEventFilter(wheelFilter);
    table->setCellWidget(row, 0, role);
    table->setCellWidget(row, 1, attribute);
    table->setCellWidget(row, 2, first);
    QComboBox *handedness = new QComboBox(table);
    handedness->addItem(QObject::tr("Tangent source W (default)"), -1);
    for(int i = 1; i < attribute->count(); i++)
      handedness->addItem(attribute->itemText(i), attribute->itemData(i));
    QComboBox *component = new QComboBox(table);
    component->addItems({lit("X"), lit("Y"), lit("Z"), lit("W")});
    component->setCurrentIndex(3);
    handedness->installEventFilter(wheelFilter);
    component->installEventFilter(wheelFilter);
    table->setCellWidget(row, 8, handedness);
    table->setCellWidget(row, 9, component);
    handedness->setEnabled(usage == FBXAttributeRole::Tangent);
    component->setEnabled(false);
    QObject::connect(
        handedness, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), &dialog,
        [=](int) {
          component->setEnabled(handedness->isEnabled() && handedness->currentData().toInt() >= 0);
          updateSave();
        });
    QObject::connect(component,
                     static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                     &dialog, [=](int) { updateSave(); });
    QObject::connect(
        role, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), &dialog,
        [=](int index) {
          handedness->setEnabled(index == int(FBXAttributeRole::Tangent));
          if(!handedness->isEnabled())
            handedness->setCurrentIndex(0);
          component->setEnabled(handedness->isEnabled() && handedness->currentData().toInt() >= 0);
        });
    for(int col = 4; col < 8; col++)
    {
      QCheckBox *option = new QCheckBox(table);
      option->setToolTip(
          col == 5
              ? QObject::tr("Normalize XYZ after range conversion; W is unchanged.")
              : QObject::tr(
                    "Convert 0..1 to -1..1 using value * 2 - 1. Off preserves the decoded value."));
      table->setCellWidget(row, col, option);
      if(col == 7)
      {
        option->setToolTip(
            QObject::tr("UV only: V = 1 - V after other conversions. U is unchanged."));
        option->setEnabled(usage == FBXAttributeRole::UV);
        QObject::connect(role, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                         &dialog, [=](int index) {
                           option->setEnabled(index == int(FBXAttributeRole::UV));
                           if(!option->isEnabled())
                             option->setChecked(false);
                         });
      }
      QObject::connect(option, &QCheckBox::toggled, &dialog, [=](bool) { updateSave(); });
    }
    table->setItem(row, 3, new QTableWidgetItem(name));
    QObject::connect(role, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                     &dialog, [=](int) { updateSave(); });
    QObject::connect(attribute,
                     static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                     &dialog, [=](int) { updateSave(); });
    QObject::connect(first, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), &dialog,
                     [=](int) { updateSave(); });
  };
  if(!selected.isEmpty())
  {
    for(const FBXAttributeMapping &m : selected)
      addMapping(m.role, m.source, m.name);
    for(int row = 0; row < selected.size(); row++)
    {
      QComboBox *handedness = static_cast<QComboBox *>(table->cellWidget(row, 8));
      handedness->setCurrentIndex(qMax(0, handedness->findData(selected[row].handednessSource)));
      static_cast<QComboBox *>(table->cellWidget(row, 9))
          ->setCurrentIndex(selected[row].handednessComponent);
      static_cast<QSpinBox *>(table->cellWidget(row, 2))->setValue(selected[row].firstComponent);
      static_cast<QCheckBox *>(table->cellWidget(row, 4))->setChecked(selected[row].remapXYZ);
      static_cast<QCheckBox *>(table->cellWidget(row, 5))->setChecked(selected[row].normalizeXYZ);
      static_cast<QCheckBox *>(table->cellWidget(row, 6))->setChecked(selected[row].remapTangentW);
      static_cast<QCheckBox *>(table->cellWidget(row, 7))->setChecked(selected[row].flipV);
    }
  }
  else
  {
    int position = -1, normal = -1, tangent = -1, binormal = -1;
    QVector<int> uvSources, colorSources;
    for(int col = 0; col < config.columns.count(); col++)
    {
      const QString name = QString(config.columns[col].name).toLower();
      if(name.contains(lit("pos")) && position < 0)
        position = col;
      else if(name.contains(lit("binormal")) || name.contains(lit("bitangent")))
        binormal = col;
      else if(name.contains(lit("tangent")))
        tangent = col;
      else if(name.contains(lit("normal")))
        normal = col;
      else if(name.contains(lit("texcoord")) || name.startsWith(lit("uv")))
        uvSources.push_back(col);
      else if(name.contains(lit("color")) || name.contains(lit("colour")))
        colorSources.push_back(col);
    }
    addMapping(FBXAttributeRole::Position, position, lit("Position"));
    addMapping(FBXAttributeRole::Normal, normal, lit("Normal"));
    addMapping(FBXAttributeRole::Tangent, tangent, lit("Tangent"));
    addMapping(FBXAttributeRole::Binormal, binormal, lit("Binormal"));
    if(uvSources.isEmpty())
      uvSources.push_back(-1);
    if(colorSources.isEmpty())
      colorSources.push_back(-1);
    for(int i = 0; i < uvSources.size(); i++)
      addMapping(FBXAttributeRole::UV, uvSources[i], lit("UV") + QString::number(i));
    for(int i = 0; i < colorSources.size(); i++)
      addMapping(FBXAttributeRole::Color, colorSources[i], lit("Color") + QString::number(i));
  }
  QPushButton *add = new QPushButton(QObject::tr("Add attribute set"), &dialog);
  QPushButton *remove = new QPushButton(QObject::tr("Remove selected rows"), &dialog);
  layout->addRow(add, remove);
  QPushButton *addRemaining =
      new QPushButton(QObject::tr("Add unmapped sources as custom data"), &dialog);
  layout->addRow(addRemaining);
  QObject::connect(addRemaining, &QPushButton::clicked, &dialog, [=, &config]() {
    QSet<int> used;
    QSet<QString> names;
    for(const FBXAttributeMapping &m : readMapping())
    {
      used.insert(m.source);
      names.insert(m.name);
    }
    for(int col = 0; col < config.columns.count(); col++)
    {
      if(used.contains(col) || config.columns[col].type.rows * config.columns[col].type.columns == 0 ||
         config.props[col].perprimitive ||
         (col < config.genericsEnabled.size() && config.genericsEnabled[col]))
        continue;
      QString name = QString(config.columns[col].name);
      if(name.isEmpty())
        name = lit("Custom");
      while(names.contains(name))
        name += lit("_");
      names.insert(name);
      addMapping(FBXAttributeRole::Custom, col, name);
    }
    updateSave();
  });
  QObject::connect(add, &QPushButton::clicked, &dialog, [=]() {
    addMapping(FBXAttributeRole::UV, -1, lit("Attribute") + QString::number(table->rowCount()));
    updateSave();
  });
  QObject::connect(remove, &QPushButton::clicked, &dialog, [=]() {
    for(int row = table->rowCount() - 1; row >= 0; row--)
      if(table->selectionModel()->isRowSelected(row, QModelIndex()))
        table->removeRow(row);
    updateSave();
  });
  QObject::connect(table, &QTableWidget::itemChanged, &dialog,
                   [=](QTableWidgetItem *) { updateSave(); });
  layout->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  updateSave();
  dialog.resize(1200, 640);
  RDDialog::show(&dialog);
  if(dialog.result() != QDialog::Accepted)
    return false;
  selected = readMapping();
  return true;
}
#endif

void ConfigureMeshOutputColumns(const PipeState &pipe, int32_t streamSelect,
                                const ShaderReflection *shader, rdcarray<ShaderConstant> &columns,
                                rdcarray<BufferElementProperties> &props)
{
  if(!shader)
    return;

  columns.reserve(shader->outputSignature.count());
  props.reserve(shader->outputSignature.count());

  int i = 0, posidx = -1;
  for(const SigParameter &sig : shader->outputSignature)
  {
    if(sig.stream != (uint32_t)streamSelect)
      continue;

    if(sig.systemValue == ShaderBuiltin::OutputIndices)
      continue;

    ShaderConstant f;
    BufferElementProperties p;

    f.name = !sig.varName.isEmpty() ? sig.varName : sig.semanticIdxName;
    if(sig.perPrimitiveRate)
      f.name += lit(" (Per-Prim)");
    f.type.rows = 1;
    f.type.columns = sig.compCount;

    p.buffer = 0;
    p.perinstance = false;
    p.perprimitive = sig.perPrimitiveRate;
    p.instancerate = 1;
    p.systemValue = sig.systemValue;
    p.format.type = ResourceFormatType::Regular;
    p.format.compByteWidth = qMax<uint32_t>(sizeof(float), VarTypeByteSize(sig.varType));
    p.format.compCount = sig.compCount;
    p.format.compType = VarTypeCompType(sig.varType);

    f.type.arrayByteStride = p.format.compByteWidth * p.format.compCount;

    if(sig.systemValue == ShaderBuiltin::Position)
      posidx = i;

    columns.push_back(f);
    props.push_back(p);

    i++;
  }

  // shift position attribute up to first, keeping order otherwise
  // the same
  if(posidx > 0)
  {
    columns.insert(0, columns.takeAt(posidx));
    props.insert(0, props.takeAt(posidx));
  }

  i = 0;
  uint32_t perPrimOffset = 0, perVertOffset = 0;
  for(i = 0; i < columns.count(); i++)
  {
    BufferElementProperties &prop = props[i];
    ShaderConstant &el = columns[i];

    uint numComps = el.type.columns;
    uint elemSize = prop.format.compByteWidth > 4 ? 8U : 4U;

    MeshDataStage outStage = MeshDataStage::VSOut;

    switch(shader->stage)
    {
      case ShaderStage::Vertex: outStage = MeshDataStage::VSOut; break;
      case ShaderStage::Hull: outStage = MeshDataStage::GSOut; break;
      case ShaderStage::Domain: outStage = MeshDataStage::GSOut; break;
      case ShaderStage::Geometry: outStage = MeshDataStage::GSOut; break;
      case ShaderStage::Task: outStage = MeshDataStage::TaskOut; break;
      case ShaderStage::Mesh: outStage = MeshDataStage::MeshOut; break;
      default: break;
    }

    uint32_t &offset = prop.perprimitive ? perPrimOffset : perVertOffset;

    if(pipe.HasAlignedPostVSData(outStage))
    {
      if(numComps == 2)
        offset = AlignUp(offset, 2U * elemSize);
      else if(numComps > 2)
        offset = AlignUp(offset, 4U * elemSize);
    }

    el.byteOffset = offset;

    offset += numComps * elemSize;
  }
}
