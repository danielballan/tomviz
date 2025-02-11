/* This source file is part of the Tomviz project, https://tomviz.org/.
   It is released under the 3-Clause BSD License, see "LICENSE". */

#include "DataSource.h"

#include "core/DataSourceBase.h"

#include "ActiveObjects.h"
#include "ColorMap.h"
#include "DataExchangeFormat.h"
#include "EmdFormat.h"
#include "GenericHDF5Format.h"
#include "ModuleFactory.h"
#include "ModuleManager.h"
#include "Operator.h"
#include "OperatorFactory.h"
#include "Pipeline.h"
#include "TimeSeriesStep.h"
#include "Utilities.h"

#include <vtkDataObject.h>
#include <vtkDoubleArray.h>
#include <vtkFieldData.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPointData.h>
#include <vtkSmartPointer.h>
#include <vtkStringArray.h>
#include <vtkTrivialProducer.h>
#include <vtkTypeInt32Array.h>
#include <vtkTypeInt8Array.h>
#include <vtkVector.h>

#include <pqTimeKeeper.h>
#include <vtkPVArrayInformation.h>
#include <vtkPVDataInformation.h>
#include <vtkPVDataSetAttributesInformation.h>
#include <vtkSMCoreUtilities.h>
#include <vtkSMParaViewPipelineController.h>
#include <vtkSMPropertyHelper.h>
#include <vtkSMSessionProxyManager.h>
#include <vtkSMSourceProxy.h>
#include <vtkSMTransferFunctionManager.h>
#include <vtkSMViewProxy.h>

#include <QDebug>
#include <QJsonArray>
#include <QMap>
#include <QTimer>

#include <cmath>
#include <cstring>
#include <sstream>

namespace {
void createOrResizeTiltAnglesArray(vtkDataObject* data)
{
  auto fd = data->GetFieldData();
  int* extent = vtkImageData::SafeDownCast(data)->GetExtent();
  int numTiltAngles = extent[5] - extent[4] + 1;
  if (!fd->HasArray("tilt_angles")) {
    vtkNew<vtkDoubleArray> array;
    array->SetName("tilt_angles");
    array->SetNumberOfTuples(numTiltAngles);
    array->FillComponent(0, 0.0);
    fd->AddArray(array);
  } else {
    // if it exists, ensure the size of the tilt angles array
    // corresponds to the size of the data
    auto array = fd->GetArray("tilt_angles");
    if (numTiltAngles != array->GetNumberOfTuples()) {
      array->SetNumberOfTuples(numTiltAngles);
    }
  }
}
} // namespace

namespace tomviz {

class DataSource::DSInternals
{
public:
  vtkNew<vtkImageData> m_transfer2D;
  vtkNew<vtkPiecewiseFunction> GradientOpacityMap;
  //  vtkSmartPointer<vtkSMSourceProxy> DataSourceProxy;
  vtkSmartPointer<vtkImageData> m_darkData;
  vtkSmartPointer<vtkImageData> m_whiteData;
  vtkSmartPointer<vtkSMSourceProxy> ProducerProxy;
  QList<Operator*> Operators;
  vtkSmartPointer<vtkSMProxy> ColorMap;
  DataSource::DataSourceType Type;
  vtkSmartPointer<vtkStringArray> Units;
  vtkVector3d DisplayPosition;
  vtkVector3d DisplayOrientation;
  PersistenceState PersistState = PersistenceState::Saved;
  vtkRectd m_transferFunction2DBox;
  bool UnitsModified = false;
  bool Forkable = true;
  // Track data array renames
  QMap<QString, QString> CurrentToOriginal;
  QList<TimeSeriesStep> timeSeriesSteps;
  int currentTimeStep = 0;

  // Checks if the tilt angles data array exists on the given VTK data
  // and creates it if it does not exist.
  void ensureTiltAnglesArrayExists()
  {
    auto alg =
      vtkAlgorithm::SafeDownCast(this->ProducerProxy->GetClientSideObject());
    Q_ASSERT(alg);
    auto data = alg->GetOutputDataObject(0);
    createOrResizeTiltAnglesArray(data);
  }
};

DataSource::DataSource(vtkSMSourceProxy* dataSource, DataSourceType dataType)
  : QObject(nullptr), Internals(new DSInternals)
{
  const char* sourceFilename = nullptr;

  QByteArray fileNameBytes;
  if (vtkSMCoreUtilities::GetFileNameProperty(dataSource)) {
    vtkSMPropertyHelper helper(
      dataSource, vtkSMCoreUtilities::GetFileNameProperty(dataSource));
    // If we are dealing with an image stack find the prefix to use
    // when displaying the data source.
    if (helper.GetNumberOfElements() > 1) {
      QStringList fileNames;
      for (unsigned int i = 0; i < helper.GetNumberOfElements(); i++) {
        fileNames << QString(helper.GetAsString(i));
      }
      QFileInfo fileInfo(fileNames[0]);
      QString fileName =
        QString("%1*.%2").arg(findPrefix(fileNames)).arg(fileInfo.suffix());
      fileNameBytes = fileName.toLatin1();
      sourceFilename = fileNameBytes.data();
    } else {
      sourceFilename = helper.GetAsString();
    }
  }

  dataSource->UpdatePipeline();
  auto algo = vtkAlgorithm::SafeDownCast(dataSource->GetClientSideObject());
  Q_ASSERT(algo);
  auto data = algo->GetOutputDataObject(0);
  auto image = vtkImageData::SafeDownCast(data);

  // Initialize our object, and set the file name.
  init(image, dataType, PersistenceState::Saved);
  setFileName(sourceFilename);
}

DataSource::DataSource(vtkImageData* data, DataSourceType dataType,
                       QObject* parentObject, PersistenceState persistState)
  : QObject(parentObject), Internals(new DSInternals)
{
  init(data, dataType, persistState);
}

DataSource::DataSource(const QString& label, DataSourceType dataType,
                       QObject* parent, PersistenceState persistState,
                       const QJsonObject& sourceInfo)
  : QObject(parent), Internals(new DSInternals)
{
  init(nullptr, dataType, persistState);

  if (!label.isNull()) {
    setLabel(label);
  }
  if (!sourceInfo.isEmpty()) {
    m_json["sourceInformation"] = sourceInfo;
  }
}

DataSource::~DataSource()
{
  if (this->Internals->ProducerProxy) {
    vtkNew<vtkSMParaViewPipelineController> controller;
    controller->UnRegisterProxy(this->Internals->ProducerProxy);
  }
  delete m_pythonProxy;
}

// Simple templated function to extend the image data.
template <typename T>
void appendImageData(vtkImageData* data, vtkImageData* slice, T* originalPtr)
{
  auto dataArray = data->GetPointData()->GetScalars();
  int extents[6];
  data->GetExtent(extents);

  // Figure out the number of bytes in the original data, and allocate a buffer.
  int bufferSize = dataArray->GetNumberOfTuples() *
                   dataArray->GetNumberOfComponents() * sizeof(T);
  unsigned char* buffer = new unsigned char[bufferSize];
  // Copy the original image data, as the allocate scalars will erase it.
  std::memcpy(buffer, originalPtr, bufferSize);
  // Now increment the z extent, and reallocate the scalar array (destructive).
  ++extents[5];
  data->SetExtent(extents);
  data->AllocateScalars(data->GetScalarType(),
                        data->GetNumberOfScalarComponents());
  // Copy the old data back into the new memory location, delete the buffer.
  auto ptr = data->GetScalarPointer();
  std::memcpy(ptr, buffer, bufferSize);
  delete[] buffer;
  buffer = 0;

  // Now copy the new slice into the array.
  void* imagePtr = data->GetScalarPointer(0, 0, extents[5]);
  auto sliceArray = slice->GetPointData()->GetScalars();
  void* slicePtr = sliceArray->GetVoidPointer(0);
  int sliceSize = sliceArray->GetNumberOfTuples() *
                  sliceArray->GetNumberOfComponents() * sizeof(T);
  std::memcpy(imagePtr, slicePtr, sliceSize);

  // Let everyone know the data has changed, then re-execute the pipeline.
  data->Modified();
}

bool DataSource::appendSlice(vtkImageData* slice)
{
  if (!slice) {
    return false;
  }

  int sliceExtents[6];
  slice->GetExtent(sliceExtents);
  auto tp = algorithm();
  if (tp) {
    auto data = vtkImageData::SafeDownCast(tp->GetOutputDataObject(0));
    if (data) {
      int extents[6];
      data->GetExtent(extents);
      cout << "The data is ";
      for (int i = 0; i < 6; ++i)
        cout << extents[i] << ", ";
      cout << endl;
      for (int i = 0; i < 4; ++i) {
        if (extents[i] != sliceExtents[i]) {
          cout << "Mismatch: " << extents[i] << " != " << sliceExtents[i]
               << endl;
          return false;
        }
      }

      // Now to append the slice onto our image data.
      switch (data->GetScalarType()) {
        vtkTemplateMacro(appendImageData(
          data, slice, static_cast<VTK_TT*>(data->GetScalarPointer())));
      }

      emit dataChanged();
      emit dataPropertiesChanged();
      pipeline()->execute()->deleteWhenFinished();
    }
  }
  return true;
}

void DataSource::setFileName(const QString& filename)
{
  QStringList fileNames = QStringList(filename);
  setFileNames(fileNames);
}

QString DataSource::fileName() const
{
  auto reader = m_json.value("reader").toObject(QJsonObject());
  if (reader.contains("fileNames")) {
    auto fileNames = reader["fileNames"].toArray();
    if (fileNames.size() > 0) {
      return fileNames[0].toString();
    }
  }
  return QString();
}

void DataSource::setFileNames(const QStringList fileNames)
{
  auto reader = m_json.value("reader").toObject(QJsonObject());
  QJsonArray files;
  foreach (QString file, fileNames) {
    files.append(file);
  }

  reader["fileNames"] = files;
  m_json["reader"] = reader;
}

void DataSource::setTvh5NodePath(const QString& path)
{
  auto reader = m_json.value("reader").toObject();
  reader["tvh5NodePath"] = path;
  m_json["reader"] = reader;
}

QString DataSource::tvh5NodePath() const
{
  return m_json.value("reader").toObject().value("tvh5NodePath").toString();
}

QStringList DataSource::fileNames() const
{
  auto reader = m_json.value("reader").toObject(QJsonObject());
  QStringList files;
  if (reader.contains("fileNames")) {
    QJsonArray fileArray = reader["fileNames"].toArray();
    foreach (QJsonValue file, fileArray) {
      files.append(file.toString());
    }
  }
  return files;
}

void DataSource::setDarkData(vtkSmartPointer<vtkImageData> image)
{
  m_pythonProxy->setDarkData(image);
  this->Internals->m_darkData = image;
}

vtkImageData* DataSource::darkData() const
{
  return this->Internals->m_darkData;
}

void DataSource::setWhiteData(vtkSmartPointer<vtkImageData> image)
{
  m_pythonProxy->setWhiteData(image);
  this->Internals->m_whiteData = image;
}

vtkImageData* DataSource::whiteData() const
{
  return this->Internals->m_whiteData;
}

bool DataSource::canReloadAndResample() const
{
  const auto& files = fileNames();

  // This currently only works for single files
  if (files.size() != 1)
    return false;

  const auto& file = files[0];

  static const QStringList h5Extensions = { "emd", "h5", "he5", "hdf5" };

  // If it looks like an HDF5 type (based on its extension), it can be
  // reloaded and resampled.
  return std::any_of(
    h5Extensions.cbegin(), h5Extensions.cend(),
    [&file](const QString& x) { return file.endsWith(x, Qt::CaseInsensitive); });
}

bool DataSource::reloadAndResample()
{
  const auto& files = fileNames();

  // This currently only works for single files
  if (files.size() != 1)
    return false;

  const auto& file = files[0];

  auto algo = vtkAlgorithm::SafeDownCast(proxy()->GetClientSideObject());
  Q_ASSERT(algo);
  auto data = algo->GetOutputDataObject(0);
  auto image = vtkImageData::SafeDownCast(data);

  bool success;
  QVariantMap options{ { "askForSubsample", true } };
  if (file.endsWith("emd", Qt::CaseInsensitive)) {
    EmdFormat format;
    success = format.read(file.toLatin1().data(), image, options);
  } else if (GenericHDF5Format::isDataExchange(file.toStdString())) {
    DataExchangeFormat format;
    success = format.read(file.toLatin1().data(), image, options);
  } else {
    success = GenericHDF5Format::read(file.toLatin1().data(), image, options);
  }

  // If there are operators, re-run the pipeline
  if (!operators().empty())
    pipeline()->execute(this, operators().first())->deleteWhenFinished();

  dataModified();
  emit activeScalarsChanged();
  emit dataPropertiesChanged();
  return success;
}

bool DataSource::isImageStack() const
{
  auto reader = m_json.value("reader").toObject(QJsonObject());
  if (reader.contains("fileNames") && reader["fileNames"].isArray()) {
    return reader["fileNames"].toArray().size() > 1;
  }
  return false;
}

void DataSource::setReaderProperties(const QVariantMap& properties)
{
  m_json["reader"] = QJsonObject::fromVariantMap(properties);
}

QVariantMap DataSource::readerProperties() const
{
  if (m_json.contains("reader") && m_json["reader"].isObject()) {
    return m_json["reader"].toObject().toVariantMap();
  } else {
    return QVariantMap();
  }
}

void DataSource::setLabel(const QString& label)
{
  m_json["label"] = label;
}

QString DataSource::label() const
{
  if (m_json.contains("label")) {
    return m_json["label"].toString();
  } else {
    return QFileInfo(fileName()).baseName();
  }
}

QString DataSource::id() const
{
  return QString().sprintf("%p", static_cast<const void*>(this));
}

template <typename T>
QJsonArray toJsonArray(T* array, int size)
{
  QJsonArray ret;
  for (int i = 0; i < size; ++i) {
    ret.append(array[i]);
  }
  return ret;
}

QJsonObject DataSource::serialize() const
{
  QJsonObject json = m_json;
  json["label"] = label();

  // If the data was subsampled, save the subsampling settings
  if (wasSubsampled()) {
    QJsonObject settings;

    int s[3];
    subsampleStrides(s);
    settings["strides"] = toJsonArray(s, 3);

    int bs[6];
    subsampleVolumeBounds(bs);
    settings["volumeBounds"] = toJsonArray(bs, 6);
    json["subsampleSettings"] = settings;
  }

  if (Internals->UnitsModified) {
    json["spacing"] = toJsonArray(getSpacing(), 3);
    if (this->Internals->Units) {
      json["units"] = this->Internals->Units->GetValue(0).c_str();
    }
  }

  json["origin"] = toJsonArray(displayPosition(), 3);
  json["orientation"] = toJsonArray(displayOrientation(), 3);

  // Serialize the currently active scalars
  json["activeScalars"] = activeScalars();
  QJsonObject scalarsRename;
  for (auto it = Internals->CurrentToOriginal.begin();
       it != Internals->CurrentToOriginal.end(); ++it) {
    scalarsRename[it.value()] = it.key();
  }
  json["scalarsRename"] = scalarsRename;

  // Serialize the color map, opacity map, and others if needed.
  json["colorOpacityMap"] = tomviz::serialize(colorMap());
  json["gradientOpacityMap"] = tomviz::serialize(gradientOpacityMap());
  QJsonObject boxJson;
  auto& transfer2DBox = this->Internals->m_transferFunction2DBox;
  boxJson["x"] = transfer2DBox.GetX();
  boxJson["y"] = transfer2DBox.GetY();
  boxJson["width"] = transfer2DBox.GetWidth();
  boxJson["height"] = transfer2DBox.GetHeight();
  json["colorMap2DBox"] = boxJson;

  // Serialize the operators...
  QJsonArray jOperators;
  foreach (Operator* op, this->Internals->Operators) {
    QJsonObject jOperator = op->serialize();
    jOperators.append(jOperator);
  }
  if (!jOperators.isEmpty()) {
    json["operators"] = jOperators;
  }

  // Serialize the modules...
  auto modules = ModuleManager::instance().findModulesGeneric(this, nullptr);
  QJsonArray jModules;
  foreach (Module* module, modules) {
    QJsonObject jModule = module->serialize();
    jModules.append(jModule);
  }
  if (!jModules.isEmpty()) {
    json["modules"] = jModules;
  }

  json["id"] = id();

  if (this == ActiveObjects::instance().activeDataSource()) {
    // Label itself as the active data source
    json["active"] = true;
  }

  return json;
}

template <typename T>
void fromJsonArray(const QJsonArray& array, T* out);

template <>
void fromJsonArray<double>(const QJsonArray& array, double* out)
{
  for (int i = 0; i < array.size(); ++i) {
    out[i] = array.at(i).toDouble();
  }
}

template <typename T>
void fromJsonArray(const QJsonValue& value, T* out)
{
  fromJsonArray(value.toArray(), out);
}

bool DataSource::deserialize(const QJsonObject& state)
{
  if (!state["label"].isUndefined()) {
    setLabel(state["label"].toString());
  }

  if (state.contains("id")) {
    ModuleManager::instance().addStateIdToDataSource(state["id"].toString(),
                                                     this);
  }

  if (state.contains("scalarsRename")) {
    auto scalarsRename = state["scalarsRename"].toObject();
    for (auto it = scalarsRename.begin(); it != scalarsRename.end(); ++it) {
      renameScalarsArray(it.key(), it.value().toString());
    }
  }

  if (state.contains("activeScalars")) {
    setActiveScalars(state["activeScalars"].toString());
  }

  if (state.contains("colorOpacityMap")) {
    tomviz::deserialize(colorMap(), state["colorOpacityMap"].toObject());
  }
  if (state.contains("gradientOpacityMap")) {
    tomviz::deserialize(gradientOpacityMap(),
                        state["gradientOpacityMap"].toObject());
  }
  if (state.contains("colorMap2DBox")) {
    auto boxJson = state["colorMap2DBox"].toObject();
    auto& transfer2DBox = this->Internals->m_transferFunction2DBox;
    transfer2DBox.Set(boxJson["x"].toDouble(), boxJson["y"].toDouble(),
                      boxJson["width"].toDouble(),
                      boxJson["height"].toDouble());
  }

  if (state.contains("spacing")) {
    double spacing[3];
    fromJsonArray(state["spacing"], spacing);
    setSpacing(spacing);
  }

  if (state.contains("units")) {
    auto units = state["units"].toString();
    setUnits(units);
  }

  if (state.contains("origin")) {
    double origin[3];
    fromJsonArray(state["origin"], origin);
    setDisplayPosition(origin);
  }

  if (state.contains("orientation")) {
    double orientation[3];
    fromJsonArray(state["orientation"], orientation);
    setDisplayOrientation(orientation);
  }

  // Check for modules on the data source first.
  if (state.contains("modules") && state["modules"].isArray()) {
    auto moduleArray = state["modules"].toArray();
    for (int i = 0; i < moduleArray.size(); ++i) {
      auto moduleObj = moduleArray[i].toObject();
      auto viewId = moduleObj["viewId"].toInt();
      auto viewProxy = ModuleManager::instance().lookupView(viewId);

      // If we can't find the view, just default the currently active view
      if (viewProxy == nullptr) {
        viewProxy = ActiveObjects::instance().activeView();
      }
      auto type = moduleObj["type"].toString();
      auto m =
        ModuleManager::instance().createAndAddModule(type, this, viewProxy);
      m->deserialize(moduleObj);
    }
  }
  // Now check for operators on the data source.
  if (state.contains("operators") && state["operators"].isArray()) {
    pipeline()->pause();
    Operator* op = nullptr;
    QJsonObject operatorObj;
    auto operatorArray = state["operators"].toArray();
    for (int i = 0; i < operatorArray.size(); ++i) {
      operatorObj = operatorArray[i].toObject();
      op = OperatorFactory::instance().createOperator(
        operatorObj["type"].toString(), this);
      if (op && op->deserialize(operatorObj)) {
        addOperator(op);
      }
    }

    // If we have a child data source we need to restore it once the data source
    // has been create by the first execution of the pipeline.
    if (op != nullptr && operatorObj.contains("dataSources")) {
      // We currently support a single child data source.
      auto dataSourcesState = operatorObj["dataSources"].toArray();
      auto connection = new QMetaObject::Connection;
      *connection = connect(pipeline(), &Pipeline::finished, op,
                            [connection, dataSourcesState, op]() {
                              auto childDataSource = op->childDataSource();
                              if (childDataSource != nullptr) {
                                childDataSource->deserialize(
                                  dataSourcesState[0].toObject());
                              }
                              QObject::disconnect(*connection);
                              delete connection;
                            });
      // If the child datasource has its own pipeline of operators increment the
      // number of pipelineFinished signals to wait for before emitting
      // stateLoaded()
      if (dataSourcesState[0].toObject().contains("operators")) {
        ModuleManager::instance().incrementPipelinesToWaitFor();
      }
    }

    if (ModuleManager::instance().executePipelinesOnLoad()) {
      pipeline()->resume();
      pipeline()->execute(this)->deleteWhenFinished();
    }
  }
  return true;
}

DataSource* DataSource::clone() const
{
  auto newClone = new DataSource(vtkImageData::SafeDownCast(this->dataObject()),
                                 this->Internals->Type, this->pipeline());
  newClone->setLabel(this->label());
  newClone->setPersistenceState(PersistenceState::Modified);

  if (this->Internals->Type == TiltSeries) {
    newClone->setTiltAngles(getTiltAngles());
  }

  QList<TimeSeriesStep> newTimeSteps;
  for (auto& timeStep : this->Internals->timeSeriesSteps) {
    newTimeSteps.append(timeStep.clone());
  }
  newClone->setTimeSeriesSteps(newTimeSteps);

  return newClone;
}

vtkSMSourceProxy* DataSource::proxy() const
{
  return this->Internals->ProducerProxy;
}

void DataSource::getExtent(int extent[6]) const
{
  vtkAlgorithm* tp = algorithm();
  if (tp) {
    vtkImageData* data = vtkImageData::SafeDownCast(tp->GetOutputDataObject(0));
    if (data) {
      data->GetExtent(extent);
      return;
    }
  }
  for (int i = 0; i < 6; ++i) {
    extent[i] = 0;
  }
}

void DataSource::getBounds(double bounds[6])
{
  vtkAlgorithm* tp = algorithm();
  if (tp) {
    vtkImageData* data = vtkImageData::SafeDownCast(tp->GetOutputDataObject(0));
    if (data) {
      data->GetBounds(bounds);
      return;
    }
  }
  for (int i = 0; i < 6; ++i) {
    bounds[i] = 0.0;
  }
}

void DataSource::getRange(vtkImageData* imageData, double range[2])
{
  for (int i = 0; i < 2; ++i) {
    range[i] = 0.0;
  }

  if (!imageData) {
    return;
  }

  auto* arrayPtr = imageData->GetPointData()->GetScalars();
  if (!arrayPtr) {
    return;
  }

  arrayPtr->GetFiniteRange(range, -1);
}

void DataSource::getSpacing(double spacing[3]) const
{
  vtkAlgorithm* tp = algorithm();
  if (tp) {
    vtkImageData* data = vtkImageData::SafeDownCast(tp->GetOutputDataObject(0));
    if (data) {
      data->GetSpacing(spacing);
      return;
    }
  }
  for (int i = 0; i < 3; ++i) {
    spacing[i] = 1;
  }
}

const double* DataSource::getSpacing() const
{
  return imageData()->GetSpacing();
}

void DataSource::setSpacing(const double spacing[3], bool markModified)
{
  if (markModified) {
    Internals->UnitsModified = true;
  }

  double mySpacing[3] = { spacing[0], spacing[1], spacing[2] };
  auto* data = imageData();
  if (data) {
    data->SetSpacing(mySpacing);
  }
  emit dataPropertiesChanged();
}

void DataSource::getPhysicalDimensions(double lengths[3]) const
{
  int extent[6];
  getExtent(extent);
  auto* spacing = getSpacing();

  for (int axis = 0; axis < 3; ++axis) {
    lengths[axis] =
      spacing[axis] * (extent[2 * axis + 1] - extent[2 * axis] + 1);
  }
}

void DataSource::setActiveScalars(const QString& arrayName)
{
  if (!getScalarsArray(arrayName)) {
    qWarning() << QString("Unable to select %1 as the active scalars array")
                    .arg(arrayName);
    return;
  }

  vtkAlgorithm* alg = algorithm();
  if (alg) {
    vtkImageData* data =
      vtkImageData::SafeDownCast(alg->GetOutputDataObject(0));
    if (data) {
      data->GetPointData()->SetActiveScalars(arrayName.toLatin1().data());
    }
  }

  dataModified();

  emit activeScalarsChanged();
  emit dataPropertiesChanged();
}

void DataSource::setActiveScalars(int arrayIdx)
{
  QStringList scalars = listScalars();

  if (arrayIdx < 0 || arrayIdx >= scalars.length()) {
    return;
  }

  setActiveScalars(scalars[arrayIdx]);
}

QString DataSource::activeScalars() const
{
  QString returnValue;
  vtkAlgorithm* alg = algorithm();
  if (alg) {
    vtkImageData* data =
      vtkImageData::SafeDownCast(alg->GetOutputDataObject(0));
    if (data) {
      vtkDataArray* scalars = data->GetPointData()->GetScalars();
      if (scalars) {
        returnValue = scalars->GetName();
      }
    }
  }

  return returnValue;
}

int DataSource::activeScalarsIdx() const
{
  QString arrayName = activeScalars();
  QStringList scalars = listScalars();
  return scalars.indexOf(arrayName);
}

QString DataSource::scalarsName(int arrayIdx) const
{
  QString arrayName;
  QStringList scalars = listScalars();

  if (arrayIdx >= 0 && arrayIdx < scalars.length()) {
    arrayName = scalars[arrayIdx];
  }

  return arrayName;
}

int DataSource::scalarsIdx(const QString& arrayName) const
{
  QStringList scalars = listScalars();
  return scalars.indexOf(arrayName);
}

QStringList DataSource::listScalars() const
{
  QStringList scalars;
  vtkAlgorithm* alg = algorithm();
  if (alg) {
    vtkImageData* data =
      vtkImageData::SafeDownCast(alg->GetOutputDataObject(0));
    if (data) {
      vtkPointData* pointData = data->GetPointData();
      auto n = pointData->GetNumberOfArrays();
      for (int i = 0; i < n; ++i) {
        scalars << pointData->GetArrayName(i);
      }
    }
  }
  return scalars;
}

void DataSource::renameScalarsArray(const QString& oldName,
                                    const QString& newName)
{
  if (oldName == newName) {
    return;
  }

  const bool isCurrentScalars = oldName == activeScalars();

  // Ensure the array actually exist
  vtkDataArray* dataArray = getScalarsArray(oldName);
  if (dataArray == nullptr) {
    return;
  }

  // Ensure the target name is not already taken
  vtkDataArray* targetArray = getScalarsArray(newName);
  if (targetArray != nullptr) {
    return;
  }

  dataArray->SetName(newName.toLatin1().data());

  if (isCurrentScalars) {
    setActiveScalars(newName);
  } else {
    dataModified();
    emit activeScalarsChanged();
    emit dataPropertiesChanged();
  }

  auto originalName = Internals->CurrentToOriginal[oldName];
  Internals->CurrentToOriginal.remove(oldName);
  Internals->CurrentToOriginal[newName] = originalName;
}

vtkDataArray* DataSource::getScalarsArray(const QString& arrayName) const
{
  vtkAlgorithm* alg = algorithm();
  if (alg == nullptr) {
    return nullptr;
  }
  vtkImageData* data = vtkImageData::SafeDownCast(alg->GetOutputDataObject(0));
  if (data == nullptr) {
    return nullptr;
  }
  vtkPointData* pointData = data->GetPointData();
  if (pointData == nullptr) {
    return nullptr;
  }
  if (pointData->HasArray(arrayName.toLatin1().data()) == 0) {
    return nullptr;
  }
  return pointData->GetScalars(arrayName.toLatin1().data());
}

unsigned int DataSource::getNumberOfComponents()
{
  unsigned int numComponents = 0;
  vtkAlgorithm* tp = this->algorithm();
  if (tp) {
    vtkImageData* data = vtkImageData::SafeDownCast(tp->GetOutputDataObject(0));
    if (data) {
      if (data->GetPointData() && data->GetPointData()->GetScalars()) {
        numComponents = static_cast<unsigned int>(
          data->GetPointData()->GetScalars()->GetNumberOfComponents());
      }
    }
  }

  return numComponents;
}

QString DataSource::getUnits()
{
  if (this->Internals->Units) {
    return QString(this->Internals->Units->GetValue(0));
  } else {
    return "nm";
  }
}

void DataSource::setUnits(const QString& units, bool markModified)
{
  if (markModified) {
    Internals->UnitsModified = true;
  }

  if (!this->Internals->Units) {
    this->Internals->Units = vtkSmartPointer<vtkStringArray>::New();
    this->Internals->Units->SetName("units");
    this->Internals->Units->SetNumberOfValues(3);
    this->Internals->Units->SetValue(0, "nm");
    this->Internals->Units->SetValue(1, "nm");
    this->Internals->Units->SetValue(2, "nm");
    vtkAlgorithm* alg = algorithm();
    if (alg) {
      vtkDataObject* data = alg->GetOutputDataObject(0);
      vtkFieldData* fd = data->GetFieldData();
      fd->AddArray(this->Internals->Units);
    }
  }
  for (int i = 0; i < 3; ++i) {
    this->Internals->Units->SetValue(i, units.toStdString().c_str());
  }
  emit dataPropertiesChanged();
}

int DataSource::addOperator(Operator* op)
{
  op->setParent(this);
  int index = this->Internals->Operators.count();
  this->Internals->Operators.push_back(op);
  emit operatorAdded(op);

  return index;
}

bool DataSource::removeOperator(Operator* op)
{
  if (op) {
    // We should emit that the operator was removed...
    this->Internals->Operators.removeAll(op);

    emit this->operatorRemoved(op);

    op->deleteLater();

    foreach (Operator* opPtr, this->Internals->Operators) {
      cout << "Operator: " << opPtr->label().toLatin1().data() << endl;
    }

    return true;
  }
  return false;
}

bool DataSource::removeAllOperators()
{
  // TODO - return false if the pipeline is running
  bool success = true;

  while (this->Internals->Operators.size() > 0) {
    Operator* lastOperator = this->Internals->Operators.takeLast();

    if (lastOperator->childDataSource() != nullptr) {
      DataSource* childDataSource = lastOperator->childDataSource();

      // Recurse on the child data source
      success = childDataSource->removeAllOperators();
      if (!success) {
        break;
      }
    }

    lastOperator->deleteLater();
  }

  if (success) {
    ModuleManager::instance().removeAllModules(this);
  }

  return success;
}

void DataSource::dataModified()
{
  vtkTrivialProducer* tp = producer();
  if (tp == nullptr) {
    return;
  }

  tp->Modified();
  vtkDataObject* dObject = tp->GetOutputDataObject(0);
  dObject->Modified();
  this->Internals->ProducerProxy->MarkModified(nullptr);

  vtkFieldData* fd = dObject->GetFieldData();
  if (fd->HasArray("tomviz_data_source_type")) {
    vtkTypeInt8Array* typeArray =
      vtkTypeInt8Array::SafeDownCast(fd->GetArray("tomviz_data_source_type"));

    // Casting is a bit hacky here, but it *should* work
    setType((DataSourceType)(int)typeArray->GetTuple1(0));
  } else {
    vtkNew<vtkTypeInt8Array> typeArray;
    typeArray->SetNumberOfComponents(1);
    typeArray->SetNumberOfTuples(1);
    typeArray->SetName("tomviz_data_source_type");
    typeArray->SetTuple1(0, this->Internals->Type);
    fd->AddArray(typeArray);
  }

  // This indirection is necessary to overcome a bug in VTK/ParaView when
  // explicitly calling UpdatePipeline(). The extents don't reset to the whole
  // extent. Until a  proper fix makes it into VTK, this is needed.
  vtkSMSessionProxyManager* pxm =
    this->Internals->ProducerProxy->GetSessionProxyManager();
  vtkSMSourceProxy* filter =
    vtkSMSourceProxy::SafeDownCast(pxm->NewProxy("filters", "PassThrough"));
  Q_ASSERT(filter);
  vtkSMPropertyHelper(filter, "Input").Set(this->Internals->ProducerProxy, 0);
  filter->UpdateVTKObjects();
  filter->UpdatePipeline();
  filter->Delete();

  emit dataChanged();
}

const QList<Operator*>& DataSource::operators() const
{
  return this->Internals->Operators;
}

void DataSource::translate(const double deltaPosition[3])
{
  for (int i = 0; i < 3; ++i) {
    this->Internals->DisplayPosition[i] += deltaPosition[i];
  }
  emit displayPositionChanged(this->Internals->DisplayPosition[0],
                              this->Internals->DisplayPosition[1],
                              this->Internals->DisplayPosition[2]);
}

const double* DataSource::displayPosition() const
{
  return this->Internals->DisplayPosition.GetData();
}

void DataSource::displayPosition(double position[3]) const
{
  auto* pos = displayPosition();
  for (int i = 0; i < 3; ++i) {
    position[i] = pos[i];
  }
}

void DataSource::setDisplayPosition(const double newPosition[3])
{
  for (int i = 0; i < 3; ++i) {
    this->Internals->DisplayPosition[i] = newPosition[i];
  }
  emit displayPositionChanged(this->Internals->DisplayPosition[0],
                              this->Internals->DisplayPosition[1],
                              this->Internals->DisplayPosition[2]);
}

const double* DataSource::displayOrientation() const
{
  return this->Internals->DisplayOrientation.GetData();
}

void DataSource::setDisplayOrientation(const double newOrientation[3])
{
  for (int i = 0; i < 3; ++i) {
    this->Internals->DisplayOrientation[i] = newOrientation[i];
  }
  emit displayOrientationChanged(this->Internals->DisplayOrientation[0],
                                 this->Internals->DisplayOrientation[1],
                                 this->Internals->DisplayOrientation[2]);
}

vtkDataObject* DataSource::copyData()
{
  this->Internals->ProducerProxy->UpdatePipeline();
  vtkDataObject* data = dataObject();
  vtkDataObject* copy = data->NewInstance();
  copy->DeepCopy(data);

  return copy;
}

void DataSource::setData(vtkDataObject* newData)
{
  auto tp = producer();
  Q_ASSERT(tp);
  tp->SetOutput(newData);
  auto fd = newData->GetFieldData();
  vtkSmartPointer<vtkTypeInt8Array> typeArray =
    vtkTypeInt8Array::SafeDownCast(fd->GetArray("tomviz_data_source_type"));
  if (typeArray && typeArray->GetTuple1(0) == TiltSeries) {
    this->Internals->ensureTiltAnglesArrayExists();
    this->Internals->Type = TiltSeries;
  } else if (typeArray && typeArray->GetTuple1(0) == FIB) {
    this->Internals->Type = FIB;
  } else {
    this->Internals->Type = Volume;
  }
  if (fd->HasArray("units")) {
    this->Internals->Units =
      vtkStringArray::SafeDownCast(fd->GetAbstractArray("units"));
  } else if (this->Internals->Units) {
    fd->AddArray(this->Internals->Units);
  }
  if (!typeArray) {
    typeArray = vtkSmartPointer<vtkTypeInt8Array>::New();
    typeArray->SetNumberOfComponents(1);
    typeArray->SetNumberOfTuples(1);
    typeArray->SetName("tomviz_data_source_type");
    fd->AddArray(typeArray);
  }
  typeArray->SetTuple1(0, this->Internals->Type);

  // Initialize maps to track array renames
  Internals->CurrentToOriginal.clear();
  auto arrayNames = listScalars();
  for (auto name : arrayNames) {
    Internals->CurrentToOriginal[name] = name;
  }

  // Make sure everything gets updated with the new data
  dataModified();
}

void DataSource::copyData(vtkDataObject* newData)
{
  auto tp = producer();
  Q_ASSERT(tp);
  auto oldData = tp->GetOutputDataObject(0);
  Q_ASSERT(oldData);

  oldData->DeepCopy(newData);

  dataModified();

  emit activeScalarsChanged();
}

void DataSource::onTimeChanged()
{
  auto* timeKeeper = ActiveObjects::instance().activeTimeKeeper();
  if (!timeKeeper) {
    return;
  }

  // Use interpolation to figure out which time step we are at
  auto numTimeSteps = numTimeSeriesSteps();
  if (numTimeSteps <= 1) {
    return;
  }

  auto timeSteps = timeKeeper->getTimeSteps();
  if (timeSteps.size() == 1) {
    return;
  } else if (timeSteps.size() == 0) {
    // It's just a 0 to 1 default.
    timeSteps.append(0);
    timeSteps.append(1);
  }

  auto timeStart = timeSteps.front();
  auto timeStop = timeSteps.back();
  auto time = timeKeeper->getTime();

  auto scale = ((numTimeSteps - 1) / (timeStop - timeStart));
  auto timeStep = std::round((time - timeStart) * scale);
  switchTimeSeriesStep(timeStep);
}

void DataSource::switchTimeSeriesStep(int i)
{
  if (i >= this->Internals->timeSeriesSteps.size()) {
    auto size = this->Internals->timeSeriesSteps.size();
    qCritical() << i
                << "is out of bounds for number of time series steps: " << size;
    return;
  }

  m_changingTimeStep = true;
  this->Internals->currentTimeStep = i;
  producer()->SetOutput(this->Internals->timeSeriesSteps[i].image);
  dataModified();
  m_changingTimeStep = false;

  emit timeStepChanged();
}

int DataSource::numTimeSeriesSteps() const
{
  return this->Internals->timeSeriesSteps.size();
}

int DataSource::currentTimeSeriesIndex() const
{
  return this->Internals->currentTimeStep;
}

void DataSource::setTimeSeriesSteps(const QList<TimeSeriesStep>& steps)
{
  this->Internals->timeSeriesSteps = steps;
  emit timeStepsModified();

  // Update the data if we need to
  auto current = currentTimeSeriesIndex();
  if (current < steps.size() && steps[current].image != imageData()) {
    // Re-use the logic here
    switchTimeSeriesStep(current);
  }
}

void DataSource::addTimeSeriesSteps(const QList<TimeSeriesStep>& steps)
{
  this->Internals->timeSeriesSteps.append(steps);
  emit timeStepsModified();
}

void DataSource::addTimeSeriesStep(const TimeSeriesStep& step)
{
  this->Internals->timeSeriesSteps.append(step);
  emit timeStepsModified();
}

TimeSeriesStep DataSource::currentTimeSeriesStep()
{
  if (!hasTimeSteps()) {
    qDebug() << "currentTimeSeriesStep() was called, but there "
             << "are no time steps!";
    return TimeSeriesStep();
  }

  auto current = this->Internals->currentTimeStep;
  if (current >= numTimeSeriesSteps()) {
    qDebug() << "Current time step is out of bounds!";
    return TimeSeriesStep();
  }

  return this->Internals->timeSeriesSteps[current];
}

QList<TimeSeriesStep> DataSource::timeSeriesSteps() const
{
  return this->Internals->timeSeriesSteps;
}

void DataSource::clearTimeSeriesSteps()
{
  this->Internals->timeSeriesSteps.clear();
  this->Internals->currentTimeStep = 0;
  emit timeStepsModified();
  emit timeStepChanged();
}

vtkSMProxy* DataSource::colorMap() const
{
  return this->Internals->ColorMap;
}

DataSource::DataSourceType DataSource::type() const
{
  return this->Internals->Type;
}

void DataSource::setType(DataSourceType t)
{
  this->Internals->Type = t;
  auto data = this->dataObject();
  setType(data, t);
  if (t == TiltSeries) {
    this->Internals->ensureTiltAnglesArrayExists();
  }
  emit dataChanged();
}

bool DataSource::hasTiltAngles()
{
  vtkDataObject* data = this->dataObject();

  return hasTiltAngles(data);
}

QVector<double> DataSource::getTiltAngles() const
{
  auto data = this->dataObject();
  return getTiltAngles(data);
}

void DataSource::setTiltAngles(const QVector<double>& angles)
{
  auto data = this->dataObject();
  setTiltAngles(data, angles);
  emit dataChanged();
}

vtkSMProxy* DataSource::opacityMap() const
{
  return this->Internals->ColorMap
           ? vtkSMPropertyHelper(this->Internals->ColorMap,
                                 "ScalarOpacityFunction")
               .GetAsProxy()
           : nullptr;
}

vtkPiecewiseFunction* DataSource::gradientOpacityMap() const
{
  return this->Internals->GradientOpacityMap;
}

vtkImageData* DataSource::transferFunction2D() const
{
  return this->Internals->m_transfer2D;
}

vtkRectd* DataSource::transferFunction2DBox() const
{
  return &this->Internals->m_transferFunction2DBox;
}

bool DataSource::hasLabelMap()
{
  auto dataSource = proxy();
  if (!dataSource) {
    return false;
  }

  // We could just as easily go to the client side VTK object to get this info,
  // but we'll go the ParaView route for now.
  vtkPVDataInformation* dataInfo = dataSource->GetDataInformation();
  vtkPVDataSetAttributesInformation* pointDataInfo =
    dataInfo->GetPointDataInformation();
  vtkPVArrayInformation* labelMapInfo =
    pointDataInfo->GetArrayInformation("LabelMap");

  return labelMapInfo != nullptr;
}

void DataSource::updateColorMap()
{
  if (m_changingTimeStep) {
    // Don't update the color map for time step changes
    return;
  }

  rescaleColorMap(colorMap(), this);
}

void DataSource::setPersistenceState(DataSource::PersistenceState state)
{
  this->Internals->PersistState = state;
}

DataSource::PersistenceState DataSource::persistenceState() const
{
  return this->Internals->PersistState;
}

vtkTrivialProducer* DataSource::producer() const
{
  Q_ASSERT(vtkTrivialProducer::SafeDownCast(proxy()->GetClientSideObject()));
  return vtkTrivialProducer::SafeDownCast(proxy()->GetClientSideObject());
}

void DataSource::init(vtkImageData* data, DataSourceType dataType,
                      PersistenceState persistState)
{
  m_pythonProxy = new DataSourceBase;
  this->Internals->Type = dataType;
  this->Internals->PersistState = persistState;
  this->Internals->DisplayPosition.Set(0, 0, 0);
  this->Internals->DisplayOrientation.Set(0, 0, 0);

  // Set up default rect for transfer function 2d...
  // The widget knows to interpret a rect with negative width as
  // uninitialized.
  this->Internals->m_transferFunction2DBox.Set(0, 0, -1, -1);

  vtkNew<vtkSMParaViewPipelineController> controller;
  auto pxm = ActiveObjects::instance().proxyManager();
  Q_ASSERT(pxm);

  // If dataSource is null then we need to create the producer
  vtkSmartPointer<vtkSMProxy> source;
  source.TakeReference(pxm->NewProxy("sources", "TrivialProducer"));
  Q_ASSERT(source != nullptr);
  Q_ASSERT(vtkSMSourceProxy::SafeDownCast(source));
  this->Internals->ProducerProxy = vtkSMSourceProxy::SafeDownCast(source);
  controller->RegisterPipelineProxy(this->Internals->ProducerProxy);

  if (data) {
    auto tp = vtkTrivialProducer::SafeDownCast(source->GetClientSideObject());
    tp->SetOutput(data);
    ensureActiveArray();
  }

  // Initialize maps to track array renames
  Internals->CurrentToOriginal.clear();
  auto arrayNames = listScalars();
  for (auto name : arrayNames) {
    Internals->CurrentToOriginal[name] = name;
  }

  // Setup color map for this data-source.
  static unsigned int colorMapCounter = 0;
  ++colorMapCounter;

  vtkNew<vtkSMTransferFunctionManager> tfmgr;
  this->Internals->ColorMap = tfmgr->GetColorTransferFunction(
    QString("DataSourceColorMap%1").arg(colorMapCounter).toLatin1().data(),
    pxm);
  ColorMap::instance().applyPreset(this->Internals->ColorMap);
  updateColorMap();

  // Every time the data changes, we should update the color map.
  connect(this, SIGNAL(dataChanged()), SLOT(updateColorMap()));

  connect(this, &DataSource::dataPropertiesChanged,
          [this]() { this->proxy()->MarkModified(nullptr); });

  auto* timeKeeper = ActiveObjects::instance().activeTimeKeeper();
  if (timeKeeper) {
    connect(timeKeeper, &pqTimeKeeper::timeChanged, this,
            &DataSource::onTimeChanged);
  }
}

vtkAlgorithm* DataSource::algorithm() const
{
  if (!proxy() || !proxy()->GetClientSideObject()) {
    return nullptr;
  }
  return vtkAlgorithm::SafeDownCast(proxy()->GetClientSideObject());
}

vtkDataObject* DataSource::dataObject() const
{
  auto alg = algorithm();
  if (!alg) {
    return nullptr;
  }
  return alg->GetOutputDataObject(0);
}

vtkImageData* DataSource::imageData() const
{
  return vtkImageData::SafeDownCast(dataObject());
}

vtkDataArray* DataSource::scalars() const
{
  return getScalarsArray(activeScalars());
}

QStringList DataSource::componentNames()
{
  ensureValidComponentNames();

  QStringList names;
  for (int i = 0; i < scalars()->GetNumberOfComponents(); ++i) {
    names.append(scalars()->GetComponentName(i));
  }

  return names;
}

void DataSource::setComponentNames(const QStringList& names)
{
  for (int i = 0; i < scalars()->GetNumberOfComponents(); ++i) {
    scalars()->SetComponentName(i, names[i].toLatin1().data());
  }
  emit componentNamesModified();
}

void DataSource::setComponentName(int index, const QString& name)
{
  scalars()->SetComponentName(index, name.toLatin1().data());
  emit componentNamesModified();
}

void DataSource::ensureValidComponentNames()
{
  bool modified = false;
  QStringList approvedNames;
  for (int i = 0; i < scalars()->GetNumberOfComponents(); ++i) {
    QString name = scalars()->GetComponentName(i);
    if (name.isEmpty() || approvedNames.contains(name)) {
      // If this name is empty or duplicated, rename it.
      size_t counter = 0;
      QString newName;
      do {
        newName = name + QString::number(++counter);
      } while (approvedNames.contains(newName));

      scalars()->SetComponentName(i, newName.toLatin1().data());
      name = newName;
      modified = true;
    }

    approvedNames.append(name);
  }

  if (modified) {
    emit componentNamesModified();
  }
}

Pipeline* DataSource::pipeline() const
{
  return qobject_cast<Pipeline*>(parent());
}

bool DataSource::unitsModified()
{
  return Internals->UnitsModified;
}

bool DataSource::isTransient() const
{
  return Internals->PersistState == PersistenceState::Transient;
}

bool DataSource::forkable()
{
  return Internals->Forkable;
}

void DataSource::setForkable(bool forkable)
{
  Internals->Forkable = forkable;
}

void DataSource::ensureActiveArray()
{
  // If there is no active array, then set one.
  if (!imageData() || !imageData()->GetPointData()) {
    return;
  }

  auto* pointData = imageData()->GetPointData();
  if (pointData->GetScalars() || pointData->GetNumberOfArrays() == 0) {
    return;
  }

  pointData->SetActiveScalars(pointData->GetArrayName(0));
}

bool DataSource::hasTiltAngles(vtkDataObject* image)
{
  vtkFieldData* fd = image->GetFieldData();

  return fd->HasArray("tilt_angles");
}

QVector<double> DataSource::getTiltAngles(vtkDataObject* data)
{
  QVector<double> result;
  auto fd = data->GetFieldData();

  if (fd->HasArray("tilt_angles")) {
    auto tiltAngles = fd->GetArray("tilt_angles");
    result.resize(tiltAngles->GetNumberOfTuples());
    for (int i = 0; i < result.size(); ++i) {
      result[i] = tiltAngles->GetTuple1(i);
    }
  }
  return result;
}

void DataSource::setTiltAngles(vtkDataObject* data,
                               const QVector<double>& angles)
{
  auto fd = data->GetFieldData();
  createOrResizeTiltAnglesArray(data);
  if (fd->HasArray("tilt_angles")) {
    auto tiltAngles = fd->GetArray("tilt_angles");
    for (int i = 0; i < tiltAngles->GetNumberOfTuples() && i < angles.size();
         ++i) {
      tiltAngles->SetTuple1(i, angles[i]);
    }
  }
}

// My attempt to reduce some of the boiler plate code in the functions below
template <typename ArrayType, typename T>
void setFieldDataArray(vtkFieldData* fd, const char* arrayName, int numTuples,
                       T* data)
{
  if (!fd->HasArray(arrayName)) {
    vtkNew<ArrayType> typeArray;
    typeArray->SetNumberOfComponents(1);
    typeArray->SetNumberOfTuples(numTuples);
    typeArray->SetName(arrayName);
    fd->AddArray(typeArray);
  }

  ArrayType* typeArray = ArrayType::SafeDownCast(fd->GetArray(arrayName));
  for (int i = 0; i < numTuples; ++i)
    typeArray->SetTuple1(i, data[i]);
}

template <typename ArrayType, typename T>
void getFieldDataArray(vtkFieldData* fd, const char* arrayName, int numTuples,
                       T* data)
{
  if (!fd->HasArray(arrayName))
    return;

  ArrayType* typeArray = ArrayType::SafeDownCast(fd->GetArray(arrayName));
  for (int i = 0; i < numTuples; ++i)
    data[i] = typeArray->GetTuple1(i);
}

void DataSource::setType(vtkDataObject* image, DataSourceType t)
{
  if (!image)
    return;

  // Cast to int before setting
  int i = static_cast<int>(t);

  const char* arrayName = "tomviz_data_source_type";
  using ArrayType = vtkTypeInt8Array;

  vtkFieldData* fd = image->GetFieldData();
  setFieldDataArray<ArrayType>(fd, arrayName, 1, &i);

  if (t != DataSourceType::TiltSeries) {
    // Clear the tilt angles
    clearTiltAngles(image);
  }
}

void DataSource::clearTiltAngles(vtkDataObject* image)
{
  if (!image)
    return;

  const char* arrayName = "tilt_angles";
  auto fd = image->GetFieldData();
  if (fd->HasArray(arrayName)) {
    fd->RemoveArray(arrayName);
  }
}

bool DataSource::wasSubsampled(vtkDataObject* image)
{
  bool ret = false;

  if (!image)
    return ret;

  const char* arrayName = "was_subsampled";
  using ArrayType = vtkTypeInt8Array;

  vtkFieldData* fd = image->GetFieldData();
  getFieldDataArray<ArrayType>(fd, arrayName, 1, &ret);
  return ret;
}

void DataSource::setWasSubsampled(vtkDataObject* image, bool b)
{
  if (!image)
    return;

  const char* arrayName = "was_subsampled";
  using ArrayType = vtkTypeInt8Array;

  vtkFieldData* fd = image->GetFieldData();
  setFieldDataArray<ArrayType>(fd, arrayName, 1, &b);
}

void DataSource::subsampleStrides(vtkDataObject* image, int s[3])
{
  // Set the default in case we return
  for (int i = 0; i < 3; ++i)
    s[i] = 1;

  if (!image)
    return;

  const char* arrayName = "subsample_strides";
  using ArrayType = vtkTypeInt32Array;

  vtkFieldData* fd = image->GetFieldData();
  getFieldDataArray<ArrayType>(fd, arrayName, 3, s);
}

void DataSource::setSubsampleStrides(vtkDataObject* image, int s[3])
{
  if (!image)
    return;

  const char* arrayName = "subsample_strides";
  using ArrayType = vtkTypeInt32Array;

  vtkFieldData* fd = image->GetFieldData();
  setFieldDataArray<ArrayType>(fd, arrayName, 3, s);
}

void DataSource::subsampleVolumeBounds(vtkDataObject* image, int bs[6])
{
  // Set the default in case we return
  for (int i = 0; i < 6; ++i)
    bs[i] = -1;

  if (!image)
    return;

  const char* arrayName = "subsample_volume_bounds";
  using ArrayType = vtkTypeInt32Array;

  vtkFieldData* fd = image->GetFieldData();
  getFieldDataArray<ArrayType>(fd, arrayName, 6, bs);
}

void DataSource::setSubsampleVolumeBounds(vtkDataObject* image, int bs[6])
{
  if (!image)
    return;

  const char* arrayName = "subsample_volume_bounds";
  using ArrayType = vtkTypeInt32Array;

  vtkFieldData* fd = image->GetFieldData();
  setFieldDataArray<ArrayType>(fd, arrayName, 6, bs);
}

} // namespace tomviz
