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

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <memory>
#include "MeshExport.h"

#ifdef RENDERDOC_FBX_SDK
namespace
{
struct BatchEvent
{
  ActionDescription action;
  QString name, group, error, signature;
  int layout = -1;
  uint32_t rows = 0;
};

struct BatchLayout
{
  BufferConfiguration config;
  QVector<FBXAttributeMapping> mapping;
  QStringList identities;
  bool configured = false;
};

// Only touch replay state here. The application's selected event and pipeline snapshot
// remain unchanged, so viewers cannot accidentally contribute another event's buffers.
struct RestoreReplayEvent
{
  IReplayController *replay;
  uint32_t event;
  ~RestoreReplayEvent() { replay->SetFrameEvent(event, true); }
};

void RunBatchWorker(QWidget *parent, const QString &title, LambdaThread &worker,
                    std::atomic<int> &progress, std::atomic<int> &event, int total,
                    std::atomic<bool> &cancel)
{
  QProgressDialog dialog(title, QObject::tr("Cancel"), 0, qMax(1, total), parent);
  dialog.setWindowTitle(title);
  dialog.setMinimumDuration(0);
  dialog.setAutoClose(false);
  dialog.setAutoReset(false);
  QObject::connect(&dialog, &QProgressDialog::canceled, [&]() { cancel = true; });
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&]() {
    dialog.setLabelText(QObject::tr("%1\nEvent %2 - %3 / %4 events")
                            .arg(title)
                            .arg(int(event))
                            .arg(int(progress))
                            .arg(total));
    dialog.setValue(progress);
    if(!worker.isRunning())
      dialog.accept();
  });
  worker.start();
  timer.start(50);
  RDDialog::show(&dialog);
  // A cancelled file finishes before its data can be freed or replay state restored.
  worker.wait();
}

QString AttributeIdentity(const BufferConfiguration &config, int col)
{
  const ShaderConstant &el = config.columns[col];
  const BufferElementProperties &p = config.props[col];
  QJsonArray key;
  key.append(QString(el.name));
  key.append(int(el.type.rows));
  key.append(int(el.type.columns));
  key.append(int(el.type.baseType));
  key.append(QString(p.format.Name()));
  key.append(p.perinstance);
  key.append(p.instancerate);
  key.append(p.perprimitive);
  key.append(int(p.systemValue));
  key.append(p.floatCastWrong);
  key.append(col < config.genericsEnabled.size() && config.genericsEnabled[col]);
  return QString::fromUtf8(QJsonDocument(key).toJson(QJsonDocument::Compact));
}

QString LayoutSignature(const BufferConfiguration &config, QStringList &identities)
{
  identities.clear();
  for(int col = 0; col < config.columns.count(); col++)
    identities.push_back(AttributeIdentity(config, col));
  QStringList sorted = identities;
  sorted.sort();
  // Ambiguous duplicate identities must never silently map to the first column.
  for(int i = 1; i < sorted.size(); i++)
    if(sorted[i] == sorted[i - 1])
      return QString();
  return QString::fromUtf8(
      QJsonDocument(QJsonArray::fromStringList(sorted)).toJson(QJsonDocument::Compact));
}

QString DecodeBatchIndices(const bytebuf &bytes, uint32_t width, uint32_t count,
                           BufferConfiguration &config)
{
  if(width != 1 && width != 2 && width != 4)
    return QObject::tr("Unsupported index width.");
  if(bytes.size() != uint64_t(width) * count)
    return QObject::tr("Truncated index buffer.");
  config.indices = new BufferData;
  config.indices->storage.resize(uint64_t(count) * sizeof(uint32_t));
  for(uint32_t i = 0; i < count; i++)
  {
    uint32_t index = 0;
    memcpy(&index, bytes.data() + uint64_t(i) * width, width);
    memcpy(config.indices->storage.data() + uint64_t(i) * sizeof(uint32_t), &index, sizeof(index));
  }
  return QString();
}

QString ReadIndices(IReplayController *r, ResourceId resource, uint64_t offset, uint32_t width,
                    uint32_t count, BufferConfiguration &config)
{
  if(resource == ResourceId())
    return QObject::tr("Index buffer is unavailable.");
  if(width != 1 && width != 2 && width != 4)
    return QObject::tr("Unsupported index width.");
  bytebuf bytes = r->GetBufferData(resource, offset, uint64_t(width) * count);
  return DecodeBatchIndices(bytes, width, count, config);
}

QString FetchBatchMesh(IReplayController *r, const ActionDescription &action, int stage,
                       uint32_t instance, bool readData, BufferConfiguration &config)
{
  const PipeState &pipe = r->GetPipelineState();
  bool meshShader = bool(action.flags & ActionFlags::MeshDispatch);
  if(!meshShader && instance >= ((action.flags & ActionFlags::Instanced) ? action.numInstances : 1U))
    return QObject::tr("Instance is out of range.");
  if(meshShader && instance != 0)
    return QObject::tr("Mesh dispatch has no draw instances.");
  config.curInstance = instance;
  if(stage == 0)
  {
    if(meshShader)
      return QObject::tr("Mesh shaders have no VS Input stage.");
    config.numRows = action.numIndices;
    config.topology = pipe.GetPrimitiveTopology();
    config.baseVertex = (action.flags & ActionFlags::Indexed) ? action.baseVertex : 0;
    BoundVBuffer ib = pipe.GetIBuffer();
    if((action.flags & ActionFlags::Indexed) && pipe.IsRestartEnabled())
    {
      config.primRestart = pipe.GetRestartIndex();
      if(ib.byteStride == 1)
        config.primRestart &= 0xff;
      if(ib.byteStride == 2)
        config.primRestart &= 0xffff;
      if(config.primRestart == 0)
        return QObject::tr("Zero-valued primitive restart is not supported.");
    }
    rdcarray<BoundVBuffer> buffers = pipe.GetVBuffers();
    for(const VertexInputAttribute &a : pipe.GetVertexInputs())
    {
      if(!a.used)
        continue;
      ShaderConstant el;
      el.name = a.name;
      el.type.rows = 1;
      el.type.columns = a.format.compCount;
      el.type.arrayByteStride = a.format.ElementSize();
      el.byteOffset = a.byteOffset;
      BufferElementProperties prop;
      prop.format = a.format;
      prop.buffer = a.vertexBuffer;
      prop.perinstance = a.perInstance;
      prop.instancerate = a.instanceRate;
      prop.floatCastWrong = a.floatCastWrong;
      if(prop.buffer >= 0 && prop.buffer < buffers.count())
      {
        uint64_t base = a.perInstance ? action.instanceOffset : action.vertexOffset;
        uint64_t offset = uint64_t(a.byteOffset) + base * buffers[prop.buffer].byteStride;
        if(offset > UINT32_MAX)
          return QObject::tr("Attribute offset exceeds supported range.");
        el.byteOffset = uint32_t(offset);
      }
      config.columns.push_back(el);
      config.props.push_back(prop);
      config.genericsEnabled.push_back(a.genericEnabled);
    }
    if(readData)
    {
      if(action.flags & ActionFlags::Indexed)
      {
        uint64_t relative = uint64_t(action.indexOffset) * ib.byteStride;
        if(relative > ib.byteSize ||
           uint64_t(action.numIndices) * ib.byteStride > ib.byteSize - relative)
          return QObject::tr("Draw exceeds the bound index range.");
        QString error = ReadIndices(r, ib.resourceId, ib.byteOffset + relative, ib.byteStride,
                                    action.numIndices, config);
        if(!error.isEmpty())
          return error;
      }
      for(int i = 0; i < buffers.count(); i++)
      {
        const BoundVBuffer &vb = buffers[i];
        BufferData *data = new BufferData;
        config.buffers.push_back(data);
        data->stride = vb.byteStride;
        bool used = false;
        for(const BufferElementProperties &prop : config.props)
          used |= prop.buffer == i;
        if(!used || vb.resourceId == ResourceId())
          continue;
        uint64_t size = 0;
        for(const BufferDescription &desc : r->GetBuffers())
          if(desc.resourceId == vb.resourceId && vb.byteOffset < desc.length)
            size = qMin(uint64_t(vb.byteSize), desc.length - vb.byteOffset);
        if(size)
          data->storage = r->GetBufferData(vb.resourceId, vb.byteOffset, size);
      }
    }
  }
  else
  {
    if(meshShader && stage == 1)
      return QObject::tr("Mesh shaders have no VS Output stage.");
    MeshDataStage outputStage = stage == 1   ? MeshDataStage::VSOut
                                : meshShader ? MeshDataStage::MeshOut
                                             : MeshDataStage::GSOut;
    const ShaderReflection *shader = NULL;
    if(stage == 1)
      shader = pipe.GetShaderReflection(ShaderStage::Vertex);
    else if(meshShader)
      shader = pipe.GetShaderReflection(ShaderStage::Mesh);
    else
    {
      shader = pipe.GetShaderReflection(ShaderStage::Geometry);
      if(!shader)
        shader = pipe.GetShaderReflection(ShaderStage::Domain);
    }
    if(!shader)
      return QObject::tr("Selected output stage is unavailable; no fallback was applied.");
    MeshFormat output = r->GetPostVSData(instance, 0, outputStage);
    if(output.vertexResourceId == ResourceId())
      return QString(output.status).isEmpty() ? QObject::tr("Output mesh is unavailable.")
                                              : QString(output.status);
    config.numRows = output.numIndices;
    config.baseVertex = output.baseVertex;
    config.topology = output.topology;
    ConfigureMeshOutputColumns(pipe, stage == 2 && !meshShader ? pipe.GetRasterizedStream() : 0,
                               shader, config.columns, config.props);
    if(stage == 1 && pipe.IsRestartEnabled() && (action.flags & ActionFlags::Indexed))
    {
      config.primRestart = pipe.GetRestartIndex();
      uint32_t width = pipe.GetIBuffer().byteStride;
      if(width == 1)
        config.primRestart &= 0xff;
      if(width == 2)
        config.primRestart &= 0xffff;
      if(config.primRestart == 0)
        return QObject::tr("Zero-valued primitive restart is not supported.");
    }
    if(readData)
    {
      if(output.indexResourceId != ResourceId())
      {
        QString error = ReadIndices(r, output.indexResourceId, output.indexByteOffset,
                                    output.indexByteStride, output.numIndices, config);
        if(!error.isEmpty())
          return error;
      }
      BufferData *data = new BufferData;
      data->stride = output.vertexByteStride;
      data->storage = r->GetBufferData(output.vertexResourceId, output.vertexByteOffset, 0);
      config.buffers.push_back(data);
    }
  }
  if(config.numRows == 0)
    return QObject::tr("No vertices.");
  if(config.topology != Topology::TriangleList && config.topology != Topology::TriangleStrip &&
     config.topology != Topology::TriangleFan)
    return QObject::tr("Topology is not a triangle list, strip or fan.");
  return QString();
}

QString SafeMeshName(QString name)
{
  name.replace(QRegularExpression(lit("[^a-zA-Z0-9_.-]+")), lit("_"));
  return name.left(80);
}

void GatherEvents(ICaptureContext &ctx, const rdcarray<ActionDescription> &actions,
                  const QString &group, QVector<BatchEvent> &events)
{
  for(const ActionDescription &action : actions)
  {
    QString name = QString(action.GetName(ctx.GetStructuredFile()));
    if(action.flags & (ActionFlags::Drawcall | ActionFlags::MeshDispatch))
    {
      BatchEvent entry;
      entry.action = action;
      entry.name = name;
      entry.group = group;
      events.push_back(entry);
    }
    GatherEvents(ctx, action.children, group.isEmpty() ? name : group + lit("/") + name, events);
  }
}

bool ParseEventRanges(const QString &text, QVector<QPair<uint32_t, uint32_t>> &ranges)
{
  ranges.clear();
  for(const QString &part : text.split(QRegularExpression(lit("[,;\\s]+")), QString::SkipEmptyParts))
  {
    QRegularExpressionMatch match = QRegularExpression(lit("^(\\d+)(?:-(\\d+))?$")).match(part);
    if(!match.hasMatch())
      return false;
    bool ok = false;
    qulonglong lo = match.captured(1).toULongLong(&ok);
    if(!ok || lo > UINT32_MAX)
      return false;
    qulonglong hi = match.captured(2).isEmpty() ? lo : match.captured(2).toULongLong(&ok);
    if(!ok || hi > UINT32_MAX || hi < lo)
      return false;
    ranges.push_back(qMakePair(uint32_t(lo), uint32_t(hi)));
  }
  return !ranges.isEmpty();
}
struct BatchOptions
{
  QString directory;
  int stage = 0;
  uint32_t instance = 0;
  bool allInstances = false, overwrite = false;
};

struct BatchResult
{
  QJsonArray files;
  int succeeded = 0, failed = 0, skipped = 0;
};

void ExecuteMeshBatch(IReplayController *r, uint32_t originalEvent, const QVector<BatchEvent> &events,
                      const QVector<std::shared_ptr<BatchLayout>> &layouts,
                      const QVector<bool> &enabled, const BatchOptions &options,
                      std::atomic<bool> &cancel, std::atomic<int> &progress,
                      std::atomic<int> &currentEvent, BatchResult &output)
{
  const QString &directory = options.directory;
  int chosenStage = options.stage;
  uint32_t chosenInstance = options.instance;
  bool all = options.allInstances, replace = options.overwrite;
  QJsonArray &results = output.files;
  int &succeeded = output.succeeded, &failed = output.failed, &skipped = output.skipped;
  RestoreReplayEvent restore{r, originalEvent};
  for(int e = 0; e < events.size(); e++)
  {
    const BatchEvent &event = events[e];
    currentEvent = int(event.action.eventId);
    uint32_t count = all && (event.action.flags & ActionFlags::Instanced)
                         ? qMax(1U, event.action.numInstances)
                         : 1;
    for(uint32_t n = 0; n < count; n++)
    {
      uint32_t inst = all ? n : chosenInstance;
      QJsonObject result;
      result[lit("eventId")] = int(event.action.eventId);
      result[lit("eventName")] = event.name;
      result[lit("group")] = event.group;
      result[lit("layout")] = event.layout >= 0 ? event.layout + 1 : 0;
      result[lit("stage")] = chosenStage;
      result[lit("instance")] = int(inst);
      QString filename =
          lit("EID_") + QString::number(event.action.eventId).rightJustified(6, QLatin1Char('0')) +
          lit("_") + SafeMeshName(event.name) + lit("_S") + QString::number(chosenStage) +
          lit("_I") + QString::number(inst) + lit(".fbx");
      result[lit("file")] = filename;
      QString error;
      bool skip = !enabled[e] || cancel;
      if(skip)
        error = cancel                  ? QObject::tr("Cancelled")
                : event.error.isEmpty() ? QObject::tr("Not selected")
                                        : event.error;
      QString path = QDir(directory).filePath(filename);
      if(!skip && QFileInfo::exists(path) && !replace)
      {
        skip = true;
        error = QObject::tr("File already exists");
      }
      if(!skip)
      {
        r->SetFrameEvent(event.action.eventId, true);
        BufferConfiguration config;
        error = FetchBatchMesh(r, event.action, chosenStage, inst, true, config);
        if(error.isEmpty())
        {
          QStringList identities;
          if(LayoutSignature(config, identities) != event.signature)
            error = QObject::tr("Layout changed since preflight.");
          else
          {
            const BatchLayout &layout = *layouts[event.layout];
            QVector<FBXAttributeMapping> selected = layout.mapping;
            QJsonArray mappingReport;
            for(FBXAttributeMapping &m : selected)
            {
              if(m.source < 0)
                continue;
              m.source = identities.indexOf(layout.identities[m.source]);
              if(m.handednessSource >= 0)
                m.handednessSource = identities.indexOf(layout.identities[m.handednessSource]);
              QJsonObject item;
              item[lit("source")] = QString(config.columns[m.source].name);
              item[lit("identity")] = identities[m.source];
              item[lit("usage")] = int(m.role);
              item[lit("name")] = m.name;
              item[lit("firstComponent")] = m.firstComponent;
              item[lit("remapXYZ")] = m.remapXYZ;
              item[lit("normalizeXYZ")] = m.normalizeXYZ;
              item[lit("remapTangentW")] = m.remapTangentW;
              item[lit("flipV")] = m.flipV;
              item[lit("handednessSourceIdentity")] =
                  m.handednessSource >= 0 ? identities[m.handednessSource] : QString();
              item[lit("handednessComponent")] = m.handednessComponent;
              mappingReport.append(item);
            }
            result[lit("mapping")] = mappingReport;
            error = WriteFBXMesh(config, config.topology, selected, path);
          }
        }
      }
      if(skip)
      {
        skipped++;
        result[lit("status")] = lit("skipped");
      }
      else if(!error.isEmpty())
      {
        failed++;
        result[lit("status")] = lit("failed");
      }
      else
      {
        succeeded++;
        result[lit("status")] = lit("success");
      }
      result[lit("reason")] = error;
      results.append(result);
    }
    progress++;
  }
}
}
#endif

void ExportMeshesBatch(ICaptureContext &ctx, QWidget *parent, const QVector<uint32_t> &initialEvents)
{
#ifndef RENDERDOC_FBX_SDK
  RDDialog::critical(parent, QObject::tr("FBX unavailable"),
                     QObject::tr("Build with the Autodesk FBX SDK to enable batch export."));
#else
  if(!ctx.IsCaptureLoaded())
    return;
  QVector<BatchEvent> catalog;
  GatherEvents(ctx, ctx.CurRootActions(), QString(), catalog);
  const uint32_t originalEvent = ctx.CurEvent();
  QDialog dialog(parent);
  dialog.setWindowTitle(QObject::tr("Batch export meshes to FBX"));
  QFormLayout *form = new QFormLayout(&dialog);
  QLineEdit *range = new QLineEdit(&dialog);
  QStringList ids;
  for(uint32_t id : initialEvents)
    ids.push_back(QString::number(id));
  range->setText(ids.join(lit(",")));
  form->addRow(QObject::tr("Event IDs / ranges (e.g. 12, 40-80)"), range);
  QComboBox *stage = new QComboBox(&dialog);
  stage->addItems({QObject::tr("VS Input"), QObject::tr("VS Output"),
                   QObject::tr("Geometry / Domain / Mesh Output")});
  form->addRow(QObject::tr("Data stage (view 0)"), stage);
  QCheckBox *allInstances = new QCheckBox(QObject::tr("Export all instances"), &dialog);
  QSpinBox *instance = new QSpinBox(&dialog);
  instance->setRange(0, INT_MAX);
  form->addRow(allInstances, instance);
  QObject::connect(allInstances, &QCheckBox::toggled, instance, &QWidget::setDisabled);
  QLabel *note = new QLabel(
      QObject::tr(
          "Preflight groups matching layouts. Select an event row and configure its layout "
          "once.\nVS Input may not contain world transforms. Unavailable stages are skipped."),
      &dialog);
  form->addRow(note);
  QTableWidget *table = new QTableWidget(0, 7, &dialog);
  table->setHorizontalHeaderLabels({QObject::tr("Export"), QObject::tr("EID"),
                                    QObject::tr("Event / group"), QObject::tr("Vertices / indices"),
                                    QObject::tr("Instances"), QObject::tr("Layout"),
                                    QObject::tr("Status")});
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  form->addRow(table);
  QPushButton *scan = new QPushButton(QObject::tr("Preflight"), &dialog);
  QPushButton *mapping = new QPushButton(QObject::tr("Configure selected layout..."), &dialog);
  form->addRow(scan, mapping);
  QCheckBox *overwrite = new QCheckBox(QObject::tr("Overwrite existing FBX files"), &dialog);
  form->addRow(overwrite);
  QDialogButtonBox *buttons =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  QVector<BatchEvent> events;
  QVector<std::shared_ptr<BatchLayout>> layouts;
  bool preflightValid = false;
  auto invalidate = [&]() {
    preflightValid = false;
    buttons->button(QDialogButtonBox::Save)->setEnabled(false);
  };
  invalidate();
  QObject::connect(range, &QLineEdit::textChanged, &dialog, invalidate);
  QObject::connect(stage, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                   &dialog, invalidate);
  QObject::connect(instance, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), &dialog,
                   invalidate);
  QObject::connect(allInstances, &QCheckBox::toggled, &dialog, invalidate);
  auto refresh = [&]() {
    bool ready = preflightValid, any = false;
    for(int i = 0; i < events.size(); i++)
    {
      const BatchEvent &event = events[i];
      bool configured = event.layout >= 0 && layouts[event.layout]->configured;
      table->item(i, 6)->setText(!event.error.isEmpty() ? event.error
                                 : configured           ? QObject::tr("Ready")
                                                        : QObject::tr("Configure mapping"));
      if(table->item(i, 0)->checkState() == Qt::Checked)
      {
        any = true;
        ready &= configured;
      }
    }
    buttons->button(QDialogButtonBox::Save)->setEnabled(ready && any);
  };
  QObject::connect(scan, &QPushButton::clicked, &dialog, [&]() {
    QVector<QPair<uint32_t, uint32_t>> ranges;
    if(!ParseEventRanges(range->text(), ranges))
    {
      RDDialog::critical(&dialog, QObject::tr("Invalid event range"),
                         QObject::tr("Enter event IDs or ascending ranges separated by commas."));
      return;
    }
    events.clear();
    layouts.clear();
    for(const BatchEvent &event : catalog)
      for(const auto &r : ranges)
        if(event.action.eventId >= r.first && event.action.eventId <= r.second)
        {
          events.push_back(event);
          break;
        }
    int chosenStage = stage->currentIndex();
    uint32_t chosenInstance = allInstances->isChecked() ? 0 : uint32_t(instance->value());
    std::atomic<bool> cancel(false);
    std::atomic<int> progress(0), currentEvent(0);
    LambdaThread worker([&]() {
      ctx.Replay().BlockInvoke([&](IReplayController *r) {
        RestoreReplayEvent restore{r, originalEvent};
        for(BatchEvent &event : events)
        {
          currentEvent = int(event.action.eventId);
          if(cancel)
          {
            event.error = QObject::tr("Preflight cancelled");
            continue;
          }
          r->SetFrameEvent(event.action.eventId, true);
          std::shared_ptr<BatchLayout> layout(new BatchLayout);
          event.error =
              FetchBatchMesh(r, event.action, chosenStage, chosenInstance, false, layout->config);
          event.rows = layout->config.numRows;
          if(event.error.isEmpty())
          {
            event.signature = LayoutSignature(layout->config, layout->identities);
            if(event.signature.isEmpty())
              event.error = QObject::tr("Ambiguous attribute identities.");
            else
            {
              for(int i = 0; i < layouts.size(); i++)
                if(event.signature == LayoutSignature(layouts[i]->config, layouts[i]->identities))
                {
                  event.layout = i;
                  break;
                }
              if(event.layout < 0)
              {
                event.layout = layouts.size();
                layouts.push_back(layout);
              }
            }
          }
          progress++;
        }
      });
    });
    RunBatchWorker(&dialog, QObject::tr("Inspecting mesh layouts"), worker, progress, currentEvent,
                   events.size(), cancel);
    QSignalBlocker blocker(table);
    table->setRowCount(events.size());
    for(int i = 0; i < events.size(); i++)
    {
      const BatchEvent &event = events[i];
      for(int col = 0; col < 7; col++)
        table->setItem(i, col, new QTableWidgetItem);
      table->item(i, 0)->setCheckState(event.error.isEmpty() ? Qt::Checked : Qt::Unchecked);
      if(!event.error.isEmpty())
        table->item(i, 0)->setFlags(Qt::ItemIsEnabled);
      table->item(i, 1)->setText(QString::number(event.action.eventId));
      table->item(i, 2)->setText(event.name + lit("\n") + event.group);
      table->item(i, 3)->setText(QString::number(event.rows));
      table->item(i, 4)->setText(QString::number(
          event.action.flags & ActionFlags::Instanced ? event.action.numInstances : 1));
      table->item(i, 5)->setText(event.layout >= 0 ? QString::number(event.layout + 1) : lit("-"));
    }
    table->resizeRowsToContents();
    preflightValid = !cancel;
    refresh();
  });
  QObject::connect(mapping, &QPushButton::clicked, &dialog, [&]() {
    int row = table->currentRow();
    if(!preflightValid || row < 0 || row >= events.size() || events[row].layout < 0)
      return;
    BatchLayout &layout = *layouts[events[row].layout];
    if(EditFBXMapping(&dialog, layout.config, layout.mapping))
      layout.configured = true;
    QSignalBlocker blocker(table);
    refresh();
  });
  QObject::connect(table, &QTableWidget::itemChanged, &dialog, [&](QTableWidgetItem *) {
    QSignalBlocker blocker(table);
    refresh();
  });
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  dialog.resize(1150, 700);
  RDDialog::show(&dialog);
  if(dialog.result() != QDialog::Accepted)
    return;
  QString directory =
      RDDialog::getExistingDirectory(parent, QObject::tr("Batch FBX output directory"));
  if(directory.isEmpty())
    return;
  QVector<bool> enabled;
  for(int i = 0; i < events.size(); i++)
    enabled.push_back(table->item(i, 0)->checkState() == Qt::Checked);
  int chosenStage = stage->currentIndex();
  uint32_t chosenInstance = instance->value();
  bool all = allInstances->isChecked(), replace = overwrite->isChecked();
  std::atomic<bool> cancel(false);
  std::atomic<int> progress(0), currentEvent(0);
  BatchOptions options;
  options.directory = directory;
  options.stage = chosenStage;
  options.instance = chosenInstance;
  options.allInstances = all;
  options.overwrite = replace;
  BatchResult output;
  LambdaThread worker([&]() {
    ctx.Replay().BlockInvoke([&](IReplayController *r) {
      ExecuteMeshBatch(r, originalEvent, events, layouts, enabled, options, cancel, progress,
                       currentEvent, output);
    });
  });
  RunBatchWorker(parent, QObject::tr("Exporting meshes"), worker, progress, currentEvent,
                 events.size(), cancel);
  QString reportPath = QDir(directory).filePath(
      lit("export_report_") + QUuid::createUuid().toString().mid(1, 36) + lit(".json"));
  QJsonObject report;
  report[lit("schemaVersion")] = 1;
  report[lit("capture")] = QString(ctx.GetCaptureFilename());
  report[lit("createdAt")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  report[lit("usageNames")] =
      QJsonArray{lit("Position"), lit("Normal"), lit("UV"),    lit("Tangent"),
                 lit("Binormal"), lit("Color"),  lit("Custom")};
  report[lit("stageNames")] =
      QJsonArray{lit("VS Input"), lit("VS Output"), lit("Geometry/Domain/Mesh Output")};
  report[lit("originalEvent")] = int(originalEvent);
  report[lit("cancelled")] = bool(cancel);
  report[lit("results")] = output.files;
  QJsonArray layoutReport;
  for(int i = 0; i < layouts.size(); i++)
  {
    QJsonObject entry;
    entry[lit("id")] = i + 1;
    entry[lit("configured")] = layouts[i]->configured;
    QJsonArray mappings;
    for(const FBXAttributeMapping &m : layouts[i]->mapping)
    {
      if(m.source < 0)
        continue;
      QJsonObject attribute;
      attribute[lit("sourceIdentity")] = layouts[i]->identities[m.source];
      attribute[lit("usage")] = int(m.role);
      attribute[lit("name")] = m.name;
      attribute[lit("firstComponent")] = m.firstComponent;
      attribute[lit("remapXYZ")] = m.remapXYZ;
      attribute[lit("normalizeXYZ")] = m.normalizeXYZ;
      attribute[lit("remapTangentW")] = m.remapTangentW;
      attribute[lit("flipV")] = m.flipV;
      attribute[lit("handednessSourceIdentity")] =
          m.handednessSource >= 0 ? layouts[i]->identities[m.handednessSource] : QString();
      attribute[lit("handednessComponent")] = m.handednessComponent;
      mappings.append(attribute);
    }
    entry[lit("mapping")] = mappings;
    layoutReport.append(entry);
  }
  report[lit("layouts")] = layoutReport;
  QSaveFile file(reportPath);
  QByteArray bytes = QJsonDocument(report).toJson();
  bool reportSaved =
      file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
  RDDialog::information(
      parent, QObject::tr("Batch FBX export finished"),
      QObject::tr("Succeeded: %1\nFailed: %2\nSkipped: %3\n%4")
          .arg(output.succeeded)
          .arg(output.failed)
          .arg(output.skipped)
          .arg(reportSaved ? reportPath
                           : QObject::tr("Could not save report: %1").arg(file.errorString())));
#endif
}

#if defined(RENDERDOC_FBX_SDK) && ENABLE_UNIT_TESTS
#include "3rdparty/catch/catch.hpp"

TEST_CASE("FBX batch layout identity and input validation", "[fbx][fbx-batch]")
{
  BufferConfiguration first, reordered;
  ShaderConstant position;
  position.name = "POSITION";
  position.type.rows = 1;
  position.type.columns = 3;
  ShaderConstant uv = position;
  uv.name = "TEXCOORD";
  uv.type.columns = 2;
  BufferElementProperties p, u;
  p.format.type = u.format.type = ResourceFormatType::Regular;
  p.format.compType = u.format.compType = CompType::Float;
  p.format.compByteWidth = u.format.compByteWidth = 4;
  p.format.compCount = 3;
  u.format.compCount = 2;
  first.columns = {position, uv};
  first.props = {p, u};
  reordered.columns = {uv, position};
  reordered.props = {u, p};
  QStringList a, b;
  CHECK((LayoutSignature(first, a) == LayoutSignature(reordered, b)));
  CHECK(b.indexOf(a[0]) == 1);
  CHECK(b.indexOf(a[1]) == 0);
  reordered.props[0].perinstance = true;
  CHECK((LayoutSignature(first, a) != LayoutSignature(reordered, b)));
  reordered.props[0].perinstance = false;
  reordered.props[0].format.compType = CompType::UInt;
  CHECK((LayoutSignature(first, a) != LayoutSignature(reordered, b)));
  first.columns.push_back(position);
  first.props.push_back(p);
  CHECK(LayoutSignature(first, a).isEmpty());
  QVector<QPair<uint32_t, uint32_t>> ranges;
  REQUIRE(ParseEventRanges(lit("12, 40-80; 100"), ranges));
  REQUIRE(ranges.size() == 3);
  CHECK(ranges[1].first == 40);
  CHECK(ranges[1].second == 80);
  CHECK_FALSE(ParseEventRanges(lit("80-40"), ranges));
  CHECK_FALSE(ParseEventRanges(lit("4294967296"), ranges));
  CHECK_FALSE(ParseEventRanges(lit("12, xyz"), ranges));
  CHECK_FALSE(ParseEventRanges(QString(), ranges));
  for(uint32_t width : {1U, 2U, 4U})
  {
    BufferConfiguration decoded;
    bytebuf bytes;
    bytes.resize(3 * width);
    uint32_t values[] = {1, 2, width == 1 ? 255U : width == 2 ? 65535U : ~0U};
    for(int i = 0; i < 3; i++)
      memcpy(bytes.data() + i * width, &values[i], width);
    REQUIRE(DecodeBatchIndices(bytes, width, 3, decoded).isEmpty());
    CHECK(memcmp(decoded.indices->data(), values, sizeof(values)) == 0);
    bytes.resize(bytes.size() - 1);
    BufferConfiguration truncated;
    CHECK_FALSE(DecodeBatchIndices(bytes, width, 3, truncated).isEmpty());
    CHECK(truncated.indices == NULL);
  }
}

TEST_CASE("FBX batch real capture replay", "[fbx-capture]")
{
  QByteArray path = qgetenv("RENDERDOC_FBX_TEST_CAPTURE");
  if(path.isEmpty())
    return;
  struct CaptureOwner
  {
    ICaptureFile *file = RENDERDOC_OpenCaptureFile();
    IReplayController *replay = NULL;
    ~CaptureOwner()
    {
      if(replay)
        replay->Shutdown();
      file->Shutdown();
    }
  } owner;
  REQUIRE(owner.file->OpenFile(path.constData(), "rdc", NULL).OK());
  auto opened = owner.file->OpenCapture(ReplayOptions(), NULL);
  REQUIRE(opened.first.OK());
  owner.replay = opened.second;
  QVector<const ActionDescription *> draws;
  std::function<void(const rdcarray<ActionDescription> &)> gather;
  gather = [&](const rdcarray<ActionDescription> &actions) {
    for(const ActionDescription &action : actions)
    {
      if(action.flags & ActionFlags::Drawcall)
        draws.push_back(&action);
      gather(action.children);
    }
  };
  gather(owner.replay->GetRootActions());
  REQUIRE(draws.size() >= 2);
  QTemporaryDir directory;
  REQUIRE(directory.isValid());
  for(int stage = 0; stage < 2; stage++)
    for(int i = 0; i < draws.size(); i++)
    {
      owner.replay->SetFrameEvent(draws[i]->eventId, true);
      BufferConfiguration config, preflight;
      const QString error = FetchBatchMesh(owner.replay, *draws[i], stage, 0, true, config);
      INFO(error.toStdString());
      REQUIRE(error.isEmpty());
      REQUIRE(FetchBatchMesh(owner.replay, *draws[i], stage, 0, false, preflight).isEmpty());
      QStringList keys, preflightKeys;
      CHECK((LayoutSignature(config, keys) == LayoutSignature(preflight, preflightKeys)));
      QVector<FBXAttributeMapping> selected = {
          {FBXAttributeRole::Position, config.guessPositionColumn(), lit("Position")}};
      CHECK(WriteFBXMesh(config, config.topology, selected,
                         directory.filePath(QString::number(stage) + lit("_") + QString::number(i) +
                                            lit(".fbx")))
                .isEmpty());
    }
  QVector<BatchEvent> events;
  QVector<std::shared_ptr<BatchLayout>> layouts;
  int expectedFiles = 0;
  for(const ActionDescription *draw : draws)
  {
    owner.replay->SetFrameEvent(draw->eventId, true);
    std::shared_ptr<BatchLayout> layout(new BatchLayout);
    REQUIRE(FetchBatchMesh(owner.replay, *draw, 0, 0, false, layout->config).isEmpty());
    layout->mapping = {
        {FBXAttributeRole::Position, layout->config.guessPositionColumn(), lit("Position")}};
    layout->configured = true;
    BatchEvent event;
    event.action = *draw;
    event.name = lit("Draw");
    event.layout = layouts.size();
    event.signature = LayoutSignature(layout->config, layout->identities);
    events.push_back(event);
    layouts.push_back(layout);
    expectedFiles += draw->flags & ActionFlags::Instanced ? draw->numInstances : 1;
  }
  BatchOptions options;
  options.directory = directory.path();
  options.allInstances = true;
  QVector<bool> enabled(events.size(), true);
  std::atomic<bool> cancel(false);
  std::atomic<int> progress(0), current(0);
  BatchResult result;
  owner.replay->SetFrameEvent(draws[0]->eventId, true);
  BufferConfiguration before;
  REQUIRE(FetchBatchMesh(owner.replay, *draws[0], 0, 0, true, before).isEmpty());
  ExecuteMeshBatch(owner.replay, draws[0]->eventId, events, layouts, enabled, options, cancel,
                   progress, current, result);
  CHECK(result.succeeded == expectedFiles);
  CHECK(result.failed == 0);
  CHECK(result.files.size() == expectedFiles);
  for(const QJsonValue &file : result.files)
  {
    CHECK(QFileInfo::exists(directory.filePath(file.toObject()[lit("file")].toString())));
    CHECK_FALSE(file.toObject()[lit("mapping")].toArray().isEmpty());
  }
  BufferConfiguration after;
  REQUIRE(FetchBatchMesh(owner.replay, *draws[0], 0, 0, true, after).isEmpty());
  REQUIRE(before.buffers.size() == after.buffers.size());
  for(int i = 0; i < before.buffers.size(); i++)
    CHECK(before.buffers[i]->storage == after.buffers[i]->storage);
  BatchResult existing;
  ExecuteMeshBatch(owner.replay, draws[0]->eventId, events, layouts, enabled, options, cancel,
                   progress, current, existing);
  CHECK(existing.skipped == expectedFiles);
  CHECK(existing.succeeded == 0);
  options.stage = 1;
  BatchResult outputInstances;
  ExecuteMeshBatch(owner.replay, draws[0]->eventId, events, layouts, enabled, options, cancel,
                   progress, current, outputInstances);
  // The layout must be preflighted for each stage; an input mapping cannot silently
  // become an output mapping just because both use a position-like attribute name.
  CHECK(outputInstances.succeeded == 0);
  CHECK(outputInstances.failed == expectedFiles);
  options.stage = 0;
  options.overwrite = true;
  layouts[0]->mapping[0].source = -1;
  BatchResult partial;
  ExecuteMeshBatch(owner.replay, draws[0]->eventId, events, layouts, enabled, options, cancel,
                   progress, current, partial);
  CHECK(partial.failed == 1);
  CHECK(partial.succeeded == expectedFiles - 1);
  cancel = true;
  BatchResult cancelled;
  ExecuteMeshBatch(owner.replay, draws[0]->eventId, events, layouts, enabled, options, cancel,
                   progress, current, cancelled);
  CHECK(cancelled.skipped == expectedFiles);
  CHECK(cancelled.succeeded == 0);
}
#endif
