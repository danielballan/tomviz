/* This source file is part of the Tomviz project, https://tomviz.org/.
   It is released under the 3-Clause BSD License, see "LICENSE". */

#ifndef tomvizModuleClip_h
#define tomvizModuleClip_h

#include "Module.h"

#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

#include <pqPropertyLinks.h>

class QComboBox;
class pqLineEdit;
class vtkActor;
class vtkPlaneSource;
class vtkPolyData;
class vtkSMProxy;
class vtkSMSourceProxy;
class vtkNonOrthoImagePlaneWidget;

namespace tomviz {

class IntSliderWidget;

class ModuleClip : public Module
{
  Q_OBJECT

public:
  ModuleClip(QObject* parent = nullptr);
  virtual ~ModuleClip();

  QString label() const override { return "Clip"; }
  QIcon icon() const override;
  using Module::initialize;
  bool initialize(DataSource* dataSource, vtkSMViewProxy* view) override;
  bool finalize() override;
  bool setVisibility(bool val) override;
  bool visibility() const override;
  QJsonObject serialize() const override;
  bool deserialize(const QJsonObject& json) override;
  bool isColorMapNeeded() const override { return true; }
  bool areScalarsMapped() const override;
  void addToPanel(QWidget* panel) override;

  void dataSourceMoved(double newX, double newY, double newZ) override;

  QString exportDataTypeString() override { return "Image"; }

  // vtkDataObject* dataToExport() override;

  enum Direction
  {
    XY = 0,
    YZ = 1,
    XZ = 2,
    Custom = 3
  };
  Q_ENUM(Direction)

protected:
  void updatePlaneWidget();
  static Direction stringToDirection(const QString& name);
  static Direction modeToDirection(int planeMode);
  vtkImageData* imageData() const;

private slots:
  void onPropertyChanged();
  void onPlaneChanged();

  void dataUpdated();

  void onDirectionChanged(Direction direction);
  void onPlaneChanged(int plane);
  void onPlaneChanged(double* point);
  int directionAxis(Direction direction);

private:
  // Should only be called from initialize after the ClipFilter has been setup.
  bool setupWidget(vtkSMViewProxy* view);

  Q_DISABLE_COPY(ModuleClip)

  vtkWeakPointer<vtkSMSourceProxy> m_clipVolume;
  vtkSmartPointer<vtkSMProxy> m_propsPanelProxy;
  vtkSmartPointer<vtkNonOrthoImagePlaneWidget> m_widget;
  vtkSmartPointer<vtkPlaneSource> m_planeSource;
  vtkSmartPointer<vtkPolyData> m_planeOutlinePolyData;
  vtkSmartPointer<vtkActor> m_planeOutlineActor;
  bool m_ignoreSignals = false;

  pqPropertyLinks m_Links;

  QPointer<QComboBox> m_directionCombo;
  QPointer<IntSliderWidget> m_planeSlider;
  Direction m_direction = Direction::XY;
  int m_plane = 0;
  int* m_extent;

  QPointer<pqLineEdit> m_pointInputs[3];
  QPointer<pqLineEdit> m_normalInputs[3];
};
} // namespace tomviz

#endif
