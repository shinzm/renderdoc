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

#include "TextureBatchExport.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
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
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QUuid>
#include <atomic>

namespace
{
struct TextureBatchScope
{
  bool pixel = true, other = false, writable = false, targets = false;
};
struct TextureBatchEntry
{
  TextureDescription texture;
  CompType cast = CompType::Typeless;
  QString name, error;
  QJsonArray references;
};
struct TextureBatchOptions
{
  QString directory;
  FileType format = FileType::DDS;
  bool firstOnly = false, overwrite = false;
};
struct TextureEventRestore
{
  IReplayController *replay;
  uint32_t event;
  ~TextureEventRestore() { replay->SetFrameEvent(event, true); }
};

QString ResourceString(ResourceId id)
{
  return QString(ToStr(id));
}

QString TextureSupportError(const TextureDescription &t, FileType format)
{
  if(t.dimension != 2 || t.depth != 1 || t.arraysize != 1 || t.cubemap || t.msSamp != 1)
    return QObject::tr("Only single-sample, non-array 2D textures are supported.");
  if(format == FileType::DDS &&
     (t.format.type == ResourceFormatType::ETC2 || t.format.type == ResourceFormatType::EAC ||
      t.format.type == ResourceFormatType::ASTC))
    return QObject::tr(
        "DDS cannot preserve this compression format; choose PNG or EXR explicitly.");
  return QString();
}

bool TextureEventRanges(const QString &text, QVector<QPair<uint32_t, uint32_t>> &ranges)
{
  ranges.clear();
  for(const QString &part : text.split(QRegularExpression(lit("[,;\\s]+")), QString::SkipEmptyParts))
  {
    auto match = QRegularExpression(lit("^(\\d+)(?:-(\\d+))?$")).match(part);
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

void TextureWorker(QWidget *parent, const QString &title, LambdaThread &worker,
                   std::atomic<int> &progress, std::atomic<bool> &cancel, int total)
{
  QProgressDialog dialog(title, QObject::tr("Cancel"), 0, qMax(1, total), parent);
  dialog.setMinimumDuration(0);
  dialog.setAutoClose(false);
  dialog.setAutoReset(false);
  QObject::connect(&dialog, &QProgressDialog::canceled, [&]() { cancel = true; });
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&]() {
    dialog.setValue(progress);
    if(!worker.isRunning())
      dialog.accept();
  });
  worker.start();
  timer.start(50);
  RDDialog::show(&dialog);
  worker.wait();
}

void FindTextureBindings(IReplayController *r, uint32_t event, const QString &eventName,
                         const TextureBatchScope &scope, QVector<TextureBatchEntry> &entries)
{
  r->SetFrameEvent(event, true);
  const PipeState &pipe = r->GetPipelineState();
  auto add = [&](const Descriptor &d, const QString &stage, const QString &kind,
                 const DescriptorAccess *access, int slot) {
    if(d.resource == ResourceId())
      return;
    const TextureDescription *texture = NULL;
    for(const TextureDescription &t : r->GetTextures())
      if(t.resourceId == d.resource)
        texture = &t;
    if(!texture)
      return;    // Buffers and empty descriptors are not textures.
    int index = -1;
    for(int i = 0; i < entries.size(); i++)
      if(entries[i].texture.resourceId == d.resource && entries[i].cast == d.format.compType)
        index = i;
    if(index < 0)
    {
      TextureBatchEntry entry;
      entry.texture = *texture;
      entry.cast = d.format.compType;
      entry.name = ResourceString(d.resource);
      entries.push_back(entry);
      index = entries.size() - 1;
    }
    QJsonObject ref;
    ref[lit("eventId")] = double(event);
    ref[lit("eventName")] = eventName;
    ref[lit("stage")] = stage;
    ref[lit("kind")] = kind;
    ref[lit("viewFormat")] = QString(d.format.Name());
    ref[lit("firstMip")] = int(d.firstMip);
    ref[lit("numMips")] = int(d.numMips);
    ref[lit("firstSlice")] = int(d.firstSlice);
    ref[lit("numSlices")] = int(d.numSlices);
    if(access)
    {
      ref[lit("reflectionIndex")] = int(access->index);
      ref[lit("arrayElement")] = double(access->arrayElement);
      ref[lit("descriptorStore")] = ResourceString(access->descriptorStore);
      ref[lit("byteOffset")] = double(access->byteOffset);
      ref[lit("byteSize")] = double(access->byteSize);
      ref[lit("usage")] = access->staticallyUnused
                              ? lit("statically unused")
                              : lit("referenced; runtime sampling not guaranteed");
      const ShaderReflection *shader = pipe.GetShaderReflection(access->stage);
      if(shader && access->index != DescriptorAccess::NoShaderBinding)
      {
        const auto &resources =
            kind == lit("input") ? shader->readOnlyResources : shader->readWriteResources;
        if(access->index < resources.size())
        {
          const ShaderResource &binding = resources[access->index];
          ref[lit("bindingName")] = QString(binding.name);
          ref[lit("bindNumber")] = double(binding.fixedBindNumber);
          ref[lit("bindSetOrSpace")] = double(binding.fixedBindSetOrSpace);
        }
      }
    }
    else
    {
      ref[lit("slot")] = slot;
      ref[lit("usage")] = lit("bound output attachment");
    }
    entries[index].references.append(ref);
  };
  for(int s = 0; s < int(ShaderStage::Count); s++)
  {
    ShaderStage stage = ShaderStage(s);
    if(stage == ShaderStage::Pixel ? !scope.pixel : !scope.other)
      continue;
    for(const UsedDescriptor &d : pipe.GetReadOnlyResources(stage))
      add(d.descriptor, QString(ToStr(stage)), lit("input"), &d.access, -1);
    if(scope.writable)
      for(const UsedDescriptor &d : pipe.GetReadWriteResources(stage))
        add(d.descriptor, QString(ToStr(stage)), lit("read-write"), &d.access, -1);
  }
  if(scope.targets)
  {
    int slot = 0;
    for(const Descriptor &d : pipe.GetOutputTargets())
      add(d, lit("Output"), lit("color"), NULL, slot++);
    add(pipe.GetDepthTarget(), lit("Output"), lit("depth"), NULL, 0);
  }
}

QString TextureFileName(const TextureBatchEntry &entry, uint32_t event, FileType format)
{
  QString name = entry.name.left(64);
  name.replace(QRegularExpression(lit("[^a-zA-Z0-9_.-]")), lit("_"));
  QString id = ResourceString(entry.texture.resourceId);
  id.replace(QRegularExpression(lit("[^a-zA-Z0-9]")), lit(""));
  QString extension = format == FileType::DDS   ? lit("dds")
                      : format == FileType::PNG ? lit("png")
                                                : lit("exr");
  return lit("%1_%2_E%3_%4.%5").arg(name).arg(id).arg(event).arg(int(entry.cast)).arg(extension);
}

QString PublishTexture(const QString &source, const QString &destination)
{
  QFile input(source);
  QSaveFile output(destination);
  if(!input.open(QIODevice::ReadOnly))
    return input.errorString();
  if(!output.open(QIODevice::WriteOnly))
    return output.errorString();
  while(!input.atEnd())
  {
    QByteArray chunk = input.read(1024 * 1024);
    if(chunk.isEmpty() && input.error() != QFile::NoError)
      return input.errorString();
    if(output.write(chunk) != chunk.size())
      return output.errorString();
  }
  return output.commit() ? QString() : output.errorString();
}

QJsonArray ExecuteTextureBatch(IReplayController *r, const QVector<TextureBatchEntry> &entries,
                               const TextureBatchOptions &options, uint32_t originalEvent,
                               std::atomic<int> &progress, std::atomic<bool> &cancel)
{
  TextureEventRestore restore = {r, originalEvent};
  QJsonArray results;
  QTemporaryDir temporary;
  // Hash the lossless mip-0 DDS, not a potentially quantized PNG/EXR. Dedup is per resource/view type.
  QMap<QString, QString> contentFiles;
  for(const TextureBatchEntry &entry : entries)
  {
    QMap<uint32_t, QJsonArray> events;
    for(const QJsonValue &value : entry.references)
    {
      uint32_t event = uint32_t(value.toObject()[lit("eventId")].toDouble());
      events[event].append(value);
    }
    QString firstPath;
    for(auto it = events.begin(); it != events.end(); ++it)
    {
      QJsonObject result;
      result[lit("resourceId")] = ResourceString(entry.texture.resourceId);
      result[lit("name")] = entry.name;
      result[lit("eventId")] = double(it.key());
      result[lit("references")] = it.value();
      result[lit("sourceFormat")] = QString(entry.texture.format.Name());
      result[lit("typeCast")] = QString(ToStr(entry.cast));
      result[lit("width")] = double(entry.texture.width);
      result[lit("height")] = double(entry.texture.height);
      result[lit("mip")] = 0;
      QString error = TextureSupportError(entry.texture, options.format);
      QString file = TextureFileName(entry, it.key(), options.format);
      QString destination = QDir(options.directory).filePath(file);
      QString status;
      if(cancel)
        status = lit("cancelled");
      else if(!error.isEmpty())
        status = lit("unsupported");
      else if(options.firstOnly && it != events.begin())
      {
        if(!firstPath.isEmpty())
        {
          file = firstPath;
          status = lit("reused-first-event");
        }
        else
        {
          status = lit("failed");
          error = QObject::tr(
              "The first event did not produce a verified file; no later snapshot was "
              "substituted.");
        }
      }
      else if(!temporary.isValid())
      {
        error = QObject::tr("Cannot create temporary directory.");
        status = lit("failed");
      }
      else
      {
        r->SetFrameEvent(it.key(), true);
        TextureSave save;
        save.resourceId = entry.texture.resourceId;
        save.typeCast = entry.cast;
        save.mip = 0;
        save.slice.sliceIndex = 0;
        QString scratch = temporary.filePath(lit("texture.dds"));
        QString key;
        // Formats DDS cannot preserve are still exportable explicitly, but not content-deduplicated.
        if(TextureSupportError(entry.texture, FileType::DDS).isEmpty())
        {
          ResultDetails saved = r->SaveTexture(save, scratch);
          if(!saved.OK())
            error = QString(saved.Message());
          else
          {
            QFile raw(scratch);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if(!raw.open(QIODevice::ReadOnly) || !hash.addData(&raw))
              error = QObject::tr("Cannot hash saved texture.");
            else
              key = ResourceString(entry.texture.resourceId) + lit(":") +
                    QString::number(int(entry.cast)) + lit(":") +
                    QString::fromLatin1(hash.result().toHex());
          }
        }
        if(!error.isEmpty())
          status = lit("failed");
        else if(!key.isEmpty() && contentFiles.contains(key))
        {
          file = contentFiles[key];
          status = lit("deduplicated");
        }
        else if(QFileInfo::exists(destination) && !options.overwrite)
          status = lit("skipped-existing");    // Never assume an existing file matches this capture.
        else
        {
          if(options.format != FileType::DDS)
          {
            save.destType = options.format;
            scratch = temporary.filePath(lit("converted"));
            ResultDetails saved = r->SaveTexture(save, scratch);
            if(!saved.OK())
              error = QString(saved.Message());
          }
          if(error.isEmpty())
            error = PublishTexture(scratch, destination);
          status = error.isEmpty() ? lit("exported") : lit("failed");
          if(error.isEmpty() && !key.isEmpty())
            contentFiles[key] = file;
        }
        if(status == lit("exported") || status == lit("deduplicated"))
          firstPath = file;
      }
      result[lit("status")] = status;
      result[lit("error")] = error;
      if(status != lit("failed") && status != lit("unsupported") && status != lit("cancelled"))
        result[lit("file")] = file;
      results.append(result);
      ++progress;
    }
  }
  return results;
}
}    // namespace

void ExportTexturesBatch(ICaptureContext &ctx, QWidget *parent, const QVector<uint32_t> &initialEvents)
{
  if(!ctx.IsCaptureLoaded())
    return;
  const uint32_t originalEvent = ctx.CurEvent();
  QMap<uint32_t, QString> catalog;
  std::function<void(const rdcarray<ActionDescription> &)> gather;
  gather = [&](const rdcarray<ActionDescription> &actions) {
    for(const ActionDescription &a : actions)
    {
      if(a.flags & (ActionFlags::Drawcall | ActionFlags::MeshDispatch | ActionFlags::Dispatch))
        catalog[a.eventId] = QString(a.GetName(ctx.GetStructuredFile()));
      gather(a.children);
    }
  };
  gather(ctx.CurRootActions());
  QDialog dialog(parent);
  dialog.setWindowTitle(QObject::tr("Batch export 2D textures"));
  QFormLayout *form = new QFormLayout(&dialog);
  QLineEdit *range = new QLineEdit(&dialog);
  QStringList ids;
  for(uint32_t event : initialEvents)
    ids << QString::number(event);
  range->setText(ids.join(lit(", ")));
  form->addRow(QObject::tr("Event IDs / ranges"), range);
  QCheckBox *pixel = new QCheckBox(QObject::tr("PS input textures"), &dialog);
  pixel->setChecked(true);
  QCheckBox *other = new QCheckBox(QObject::tr("Other shader stages (including CS)"), &dialog);
  QCheckBox *writable =
      new QCheckBox(QObject::tr("Include read-write textures in selected stages"), &dialog);
  QCheckBox *targets = new QCheckBox(QObject::tr("Include color / depth attachments"), &dialog);
  form->addRow(pixel, other);
  form->addRow(writable, targets);
  QComboBox *format = new QComboBox(&dialog);
  format->addItems({QObject::tr("DDS - preserve resource format"),
                    QObject::tr("PNG - convert to 8-bit image"),
                    QObject::tr("EXR - convert to floating-point image")});
  form->addRow(QObject::tr("Format"), format);
  QCheckBox *first = new QCheckBox(
      QObject::tr("Export each resource only at its first selected event (faster)"), &dialog);
  form->addRow(first);
  QLabel *note = new QLabel(
      QObject::tr(
          "Only ordinary 2D textures, mip 0. No image flip or viewer display adjustments.\n"
          "All snapshots are after the event. Shader references do not prove runtime sampling.\n"
          "Default: compare lossless contents across events. PNG/EXR explicitly convert format; "
          "DDS is recommended for original data.\n"
          "Exports resource mip 0 even if a bound view starts at another mip. Arrays, Cube, 3D and "
          "MSAA are skipped."),
      &dialog);
  note->setWordWrap(true);
  form->addRow(note);
  QTableWidget *table = new QTableWidget(0, 6, &dialog);
  table->setHorizontalHeaderLabels({QObject::tr("Export"), QObject::tr("Texture / ID"),
                                    QObject::tr("Size"), QObject::tr("Format / view type"),
                                    QObject::tr("References"), QObject::tr("Status")});
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  form->addRow(table);
  QTextEdit *references = new QTextEdit(&dialog);
  references->setReadOnly(true);
  references->setMaximumHeight(150);
  form->addRow(QObject::tr("Selected texture bindings"), references);
  QPushButton *scan = new QPushButton(QObject::tr("Preflight"), &dialog);
  form->addRow(scan);
  QCheckBox *overwrite = new QCheckBox(QObject::tr("Overwrite existing texture files"), &dialog);
  form->addRow(overwrite);
  QDialogButtonBox *buttons =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  QVector<TextureBatchEntry> entries;
  bool valid = false;
  auto refreshSave = [&]() {
    bool selected = false;
    for(int row = 0; row < table->rowCount(); row++)
      selected |= table->item(row, 0)->checkState() == Qt::Checked;
    buttons->button(QDialogButtonBox::Save)->setEnabled(valid && selected);
  };
  auto invalidate = [&]() {
    valid = false;
    refreshSave();
  };
  QObject::connect(range, &QLineEdit::textChanged, &dialog, invalidate);
  for(QCheckBox *box : {pixel, other, writable, targets})
    QObject::connect(box, &QCheckBox::toggled, &dialog, invalidate);
  QObject::connect(format, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                   &dialog, invalidate);
  QObject::connect(table, &QTableWidget::itemChanged, &dialog, refreshSave);
  QObject::connect(table, &QTableWidget::itemSelectionChanged, &dialog, [&]() {
    int row = table->currentRow();
    if(row >= 0 && row < entries.size())
    {
      QStringList lines;
      for(const QJsonValue &value : entries[row].references)
      {
        QJsonObject ref = value.toObject();
        QString binding = ref[lit("bindingName")].toString();
        if(ref.contains(lit("bindNumber")))
          binding += QObject::tr(" (binding %1, set/space %2, element %3)")
                         .arg(ref[lit("bindNumber")].toInt())
                         .arg(ref[lit("bindSetOrSpace")].toInt())
                         .arg(ref[lit("arrayElement")].toInt());
        else if(ref.contains(lit("descriptorStore")))
          binding = QObject::tr("%1, descriptor offset %2")
                        .arg(ref[lit("descriptorStore")].toString())
                        .arg(ref[lit("byteOffset")].toInt());
        else
          binding = QObject::tr("slot %1").arg(ref[lit("slot")].toInt());
        lines << QObject::tr("EID %1 | %2 | %3 %4 | %5 | view %6, mip %7 (%8 mips) | %9")
                     .arg(ref[lit("eventId")].toInt())
                     .arg(ref[lit("eventName")].toString())
                     .arg(ref[lit("stage")].toString())
                     .arg(ref[lit("kind")].toString())
                     .arg(binding)
                     .arg(ref[lit("viewFormat")].toString())
                     .arg(ref[lit("firstMip")].toInt())
                     .arg(ref[lit("numMips")].toInt())
                     .arg(ref[lit("usage")].toString());
      }
      references->setPlainText(lines.join(lit("\n")));
    }
  });
  auto selectedFormat = [&]() {
    return format->currentIndex() == 0   ? FileType::DDS
           : format->currentIndex() == 1 ? FileType::PNG
                                         : FileType::EXR;
  };
  QObject::connect(scan, &QPushButton::clicked, &dialog, [&]() {
    invalidate();
    QVector<QPair<uint32_t, uint32_t>> ranges;
    if(!TextureEventRanges(range->text(), ranges))
    {
      RDDialog::critical(&dialog, QObject::tr("Invalid events"),
                         QObject::tr("Enter event IDs or ranges, for example 12, 40-80."));
      return;
    }
    QMap<uint32_t, QString> events;
    for(auto it = catalog.begin(); it != catalog.end(); ++it)
      for(const auto &interval : ranges)
        if(it.key() >= interval.first && it.key() <= interval.second)
          events[it.key()] = it.value();
    TextureBatchScope scope;
    scope.pixel = pixel->isChecked();
    scope.other = other->isChecked();
    scope.writable = writable->isChecked();
    scope.targets = targets->isChecked();
    entries.clear();
    table->blockSignals(true);
    table->setRowCount(0);
    references->clear();
    std::atomic<int> progress(0);
    std::atomic<bool> cancel(false);
    LambdaThread worker([&]() {
      ctx.Replay().BlockInvoke([&](IReplayController *r) {
        TextureEventRestore restore = {r, originalEvent};
        for(auto it = events.begin(); it != events.end() && !cancel; ++it)
        {
          FindTextureBindings(r, it.key(), it.value(), scope, entries);
          ++progress;
        }
      });
    });
    TextureWorker(&dialog, QObject::tr("Finding bound textures"), worker, progress, cancel,
                  events.size());
    if(cancel)
      entries.clear();
    for(TextureBatchEntry &entry : entries)
    {
      entry.name = QString(ctx.GetResourceName(entry.texture.resourceId));
      entry.error = TextureSupportError(entry.texture, selectedFormat());
      int row = table->rowCount();
      table->insertRow(row);
      QTableWidgetItem *check = new QTableWidgetItem;
      check->setFlags(entry.error.isEmpty()
                          ? Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable
                          : Qt::ItemIsSelectable);
      check->setCheckState(entry.error.isEmpty() ? Qt::Checked : Qt::Unchecked);
      table->setItem(row, 0, check);
      table->setItem(
          row, 1,
          new QTableWidgetItem(entry.name + lit(" / ") + ResourceString(entry.texture.resourceId)));
      table->setItem(
          row, 2,
          new QTableWidgetItem(lit("%1 x %2").arg(entry.texture.width).arg(entry.texture.height)));
      table->setItem(row, 3,
                     new QTableWidgetItem(QString(entry.texture.format.Name()) + lit(" / ") +
                                          QString(ToStr(entry.cast))));
      table->setItem(row, 4, new QTableWidgetItem(QString::number(entry.references.size())));
      table->setItem(
          row, 5, new QTableWidgetItem(entry.error.isEmpty() ? QObject::tr("Ready") : entry.error));
    }
    table->blockSignals(false);
    valid = !cancel;
    refreshSave();
    if(entries.isEmpty() && !cancel)
      references->setPlainText(
          QObject::tr("No texture bindings found. Check event ranges and shader stages; compute "
                      "dispatches require Other shader stages."));
  });
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  refreshSave();
  dialog.resize(1150, 800);
  if(RDDialog::show(&dialog) != QDialog::Accepted)
    return;
  TextureBatchOptions options;
  options.directory =
      RDDialog::getExistingDirectory(parent, QObject::tr("Batch texture output directory"));
  if(options.directory.isEmpty())
    return;
  options.format = selectedFormat();
  options.firstOnly = first->isChecked();
  options.overwrite = overwrite->isChecked();
  QVector<TextureBatchEntry> selected;
  int total = 0;
  for(int row = 0; row < entries.size(); row++)
    if(table->item(row, 0)->checkState() == Qt::Checked)
    {
      selected.push_back(entries[row]);
      QSet<uint32_t> events;
      for(const QJsonValue &ref : entries[row].references)
        events.insert(uint32_t(ref.toObject()[lit("eventId")].toDouble()));
      total += events.size();
    }
  std::atomic<int> progress(0);
  std::atomic<bool> cancel(false);
  QJsonArray results;
  LambdaThread worker([&]() {
    ctx.Replay().BlockInvoke([&](IReplayController *r) {
      results = ExecuteTextureBatch(r, selected, options, originalEvent, progress, cancel);
    });
  });
  TextureWorker(parent, QObject::tr("Exporting 2D textures"), worker, progress, cancel, total);
  QJsonObject report;
  report[lit("schemaVersion")] = 1;
  report[lit("capture")] = QString(ctx.GetCaptureFilename());
  report[lit("createdAt")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  report[lit("originalEvent")] = double(originalEvent);
  report[lit("snapshot")] = lit("after event");
  report[lit("mip")] = 0;
  report[lit("format")] = QString(ToStr(options.format));
  report[lit("firstEventOnly")] = options.firstOnly;
  report[lit("cancelled")] = bool(cancel);
  report[lit("results")] = results;
  QJsonArray omitted;
  for(int row = 0; row < entries.size(); row++)
    if(table->item(row, 0)->checkState() != Qt::Checked)
    {
      QJsonObject item;
      item[lit("resourceId")] = ResourceString(entries[row].texture.resourceId);
      item[lit("reason")] = entries[row].error.isEmpty() ? lit("unchecked") : entries[row].error;
      item[lit("references")] = entries[row].references;
      omitted.append(item);
    }
  report[lit("omitted")] = omitted;
  QString reportPath =
      QDir(options.directory)
          .filePath(
              lit("texture_export_report_%1.json").arg(QUuid::createUuid().toString().mid(1, 36)));
  QSaveFile reportFile(reportPath);
  QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
  bool reportOK = reportFile.open(QIODevice::WriteOnly) && reportFile.write(json) == json.size() &&
                  reportFile.commit();
  QMap<QString, int> counts;
  for(const QJsonValue &value : results)
    counts[value.toObject()[lit("status")].toString()]++;
  QStringList summary;
  for(auto it = counts.begin(); it != counts.end(); ++it)
    summary << lit("%1: %2").arg(it.key()).arg(it.value());
  summary << (reportOK
                  ? QObject::tr("Report: %1").arg(reportPath)
                  : QObject::tr("Report could not be written: %1").arg(reportFile.errorString()));
  RDDialog::information(parent, QObject::tr("Texture export finished"), summary.join(lit("\n")));
}

#if ENABLE_UNIT_TESTS
#include <QImage>
#include "3rdparty/catch/catch.hpp"

TEST_CASE("Texture batch scope rejects special textures and parses event ranges", "[texture-batch]")
{
  TextureDescription t = {};
  t.dimension = 2;
  t.depth = t.arraysize = t.msSamp = 1;
  CHECK((TextureSupportError(t, FileType::DDS).isEmpty()));
  SECTION("array")
  {
    t.arraysize = 2;
  }
  SECTION("cube")
  {
    t.cubemap = true;
  }
  SECTION("volume")
  {
    t.dimension = 3;
  }
  SECTION("MSAA")
  {
    t.msSamp = 4;
  }
  REQUIRE_FALSE(TextureSupportError(t, FileType::DDS).isEmpty());
  QVector<QPair<uint32_t, uint32_t>> ranges;
  CHECK((TextureEventRanges(lit("12, 40-80; 90"), ranges)));
  CHECK((ranges.size() == 3));
  CHECK_FALSE(TextureEventRanges(lit("80-40"), ranges));
  CHECK_FALSE(TextureEventRanges(lit("4294967296"), ranges));
  CHECK_FALSE(TextureEventRanges(lit("12, nope"), ranges));
}

TEST_CASE("Texture batch capture preserves mip zero, snapshots and references",
          "[texture-batch-capture]")
{
  QByteArray path = qgetenv("RENDERDOC_TEXTURE_TEST_CAPTURE");
  if(path.isEmpty())
  {
    WARN("Set RENDERDOC_TEXTURE_TEST_CAPTURE to the texture batch fixture capture.");
    return;
  }
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
  REQUIRE((owner.file->OpenFile(path.constData(), "rdc", NULL).OK()));
  auto opened = owner.file->OpenCapture(ReplayOptions(), NULL);
  REQUIRE((opened.first.OK()));
  owner.replay = opened.second;
  QVector<uint32_t> draws;
  std::function<void(const rdcarray<ActionDescription> &)> gather;
  gather = [&](const rdcarray<ActionDescription> &actions) {
    for(const ActionDescription &action : actions)
    {
      if(action.flags & ActionFlags::Drawcall)
        draws << action.eventId;
      gather(action.children);
    }
  };
  gather(owner.replay->GetRootActions());
  REQUIRE((draws.size() == 3));
  TextureBatchScope scope;
  QVector<TextureBatchEntry> entries;
  for(uint32_t event : draws)
    FindTextureBindings(owner.replay, event, lit("fixture draw"), scope, entries);
  REQUIRE((entries.size() == 3));
  QVector<TextureBatchEntry> allEntries = entries;
  entries.clear();
  for(const TextureBatchEntry &entry : allEntries)
    if(entry.texture.mips == 2)
      entries.push_back(entry);
  REQUIRE((entries.size() == 1));
  REQUIRE((entries[0].references.size() == 6));
  CHECK((entries[0].texture.mips == 2));
  CHECK((entries[0].references[0].toObject()[lit("bindNumber")].toInt() == 0));
  CHECK((entries[0].references[1].toObject()[lit("bindNumber")].toInt() == 1));
  owner.replay->SetFrameEvent(draws[0], true);
  bytebuf before = owner.replay->GetTextureData(entries[0].texture.resourceId, Subresource());
  QTemporaryDir directory;
  REQUIRE((directory.isValid()));
  TextureBatchOptions options;
  options.directory = directory.path();
  std::atomic<int> progress(0);
  std::atomic<bool> cancel(false);
  SECTION("compressed sRGB and float resources preserve DDS data")
  {
    for(const TextureBatchEntry &entry : allEntries)
    {
      owner.replay->SetFrameEvent(draws[0], true);
      bytebuf original = owner.replay->GetTextureData(entry.texture.resourceId, Subresource());
      auto result = ExecuteTextureBatch(owner.replay, {entry}, options, draws[0], progress, cancel);
      REQUIRE((result[0].toObject()[lit("status")] == lit("exported")));
      QFile file(QDir(directory.path()).filePath(result[0].toObject()[lit("file")].toString()));
      REQUIRE((file.open(QIODevice::ReadOnly)));
      QByteArray bytes = file.readAll();
      CHECK((bytes.right(int(original.size())) ==
             QByteArray(reinterpret_cast<const char *>(original.data()), int(original.size()))));
      if(entry.texture.format.type == ResourceFormatType::BC1)
      {
        CHECK((entry.texture.format.SRGBCorrected()));
        REQUIRE((bytes.mid(84, 4) == QByteArray("DX10")));
        uint32_t format = 0;
        memcpy(&format, bytes.constData() + 128, 4);
        CHECK((format == 72));    // DXGI_FORMAT_BC1_UNORM_SRGB.
      }
    }
  }
  SECTION("depth and color attachments")
  {
    scope.pixel = false;
    scope.targets = true;
    QVector<TextureBatchEntry> outputs;
    FindTextureBindings(owner.replay, draws[0], lit("fixture draw"), scope, outputs);
    REQUIRE((outputs.size() == 2));
    auto result = ExecuteTextureBatch(owner.replay, outputs, options, draws[0], progress, cancel);
    for(const QJsonValue &value : result)
      CHECK((value.toObject()[lit("status")] == lit("exported")));
  }
  SECTION("DDS snapshots")
  {
    QJsonArray result =
        ExecuteTextureBatch(owner.replay, entries, options, draws[0], progress, cancel);
    REQUIRE((result.size() == 3));
    CHECK((result[0].toObject()[lit("status")] == lit("exported")));
    CHECK((result[1].toObject()[lit("status")] == lit("exported")));
    CHECK((result[2].toObject()[lit("status")] == lit("deduplicated")));
    CHECK((result[1].toObject()[lit("file")] == result[2].toObject()[lit("file")]));
    QFile file(QDir(directory.path()).filePath(result[0].toObject()[lit("file")].toString()));
    REQUIRE((file.open(QIODevice::ReadOnly)));
    QByteArray bytes = file.readAll();
    REQUIRE((bytes.size() >= 144));
    CHECK((bytes.left(4) == QByteArray("DDS ")));
    uint32_t mipCount = 0;
    memcpy(&mipCount, bytes.constData() + 28, 4);
    CHECK((mipCount == 1));
    CHECK((bytes.right(16) == QByteArray(reinterpret_cast<const char *>(before.data()), 16)));
  }
  SECTION("PNG orientation and contents")
  {
    options.format = FileType::PNG;
    auto result = ExecuteTextureBatch(owner.replay, entries, options, draws[0], progress, cancel);
    QImage first(QDir(directory.path()).filePath(result[0].toObject()[lit("file")].toString()));
    REQUIRE((first.size() == QSize(2, 2)));
    CHECK((first.pixel(0, 0) == qRgba(255, 0, 0, 255)));
    CHECK((first.pixel(0, 1) == qRgba(0, 0, 255, 255)));
    QImage second(QDir(directory.path()).filePath(result[1].toObject()[lit("file")].toString()));
    CHECK((second.pixel(0, 0) == qRgba(255, 255, 0, 255)));
  }
  SECTION("EXR")
  {
    options.format = FileType::EXR;
    auto result = ExecuteTextureBatch(owner.replay, entries, options, draws[0], progress, cancel);
    CHECK((result[0].toObject()[lit("status")] == lit("exported")));
    QFile file(QDir(directory.path()).filePath(result[0].toObject()[lit("file")].toString()));
    REQUIRE((file.open(QIODevice::ReadOnly)));
    CHECK((file.read(4).toHex() == QByteArray("762f3101")));
  }
  SECTION("first event only")
  {
    options.firstOnly = true;
    auto result = ExecuteTextureBatch(owner.replay, entries, options, draws[0], progress, cancel);
    CHECK((result[0].toObject()[lit("status")] == lit("exported")));
    CHECK((result[1].toObject()[lit("status")] == lit("reused-first-event")));
    CHECK((result[0].toObject()[lit("file")] == result[2].toObject()[lit("file")]));
  }
  SECTION("existing files and overwrite")
  {
    QString destination =
        QDir(directory.path()).filePath(TextureFileName(entries[0], draws[0], FileType::DDS));
    QFile sentinel(destination);
    REQUIRE((sentinel.open(QIODevice::WriteOnly)));
    sentinel.write("preserve");
    sentinel.close();
    auto result = ExecuteTextureBatch(owner.replay, entries, options, draws[0], progress, cancel);
    CHECK((result[0].toObject()[lit("status")] == lit("skipped-existing")));
    REQUIRE((sentinel.open(QIODevice::ReadOnly)));
    CHECK((sentinel.readAll() == "preserve"));
    sentinel.close();
    options.overwrite = true;
    result = ExecuteTextureBatch(owner.replay, entries, options, draws[0], progress, cancel);
    CHECK((result[0].toObject()[lit("status")] == lit("exported")));
  }
  SECTION("cancellation")
  {
    cancel = true;
    auto result = ExecuteTextureBatch(owner.replay, entries, options, draws[0], progress, cancel);
    REQUIRE((result.size() == 3));
    for(const QJsonValue &value : result)
      CHECK((value.toObject()[lit("status")] == lit("cancelled")));
    CHECK((QDir(directory.path()).entryList(QDir::Files).isEmpty()));
  }
  SECTION("failure continues")
  {
    REQUIRE((QDir(directory.path()).mkdir(TextureFileName(entries[0], draws[0], FileType::DDS))));
    options.overwrite = true;
    auto result = ExecuteTextureBatch(owner.replay, entries, options, draws[0], progress, cancel);
    CHECK((result[0].toObject()[lit("status")] == lit("failed")));
    CHECK((result[1].toObject()[lit("status")] == lit("exported")));
  }
  CHECK((owner.replay->GetTextureData(entries[0].texture.resourceId, Subresource()) == before));
}
#endif
