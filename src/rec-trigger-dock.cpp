#include "rec-trigger-dock.h"
#include "camera-tally-filter.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <graphics/matrix4.h>
#include <graphics/vec3.h>

#include <QCheckBox>
#include <QAbstractButton>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDate>
#include <QDateTime>
#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QPointer>
#include <QPushButton>
#include <QPixmap>
#include <QRectF>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStringList>
#include <QVariant>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <util/bmem.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>

extern "C" {
const char *config_get_string(config_t *config, const char *section, const char *name);
long long config_get_int(config_t *config, const char *section, const char *name);
unsigned long long config_get_uint(config_t *config, const char *section, const char *name);
void config_set_string(config_t *config, const char *section, const char *name, const char *value);
void config_set_bool(config_t *config, const char *section, const char *name, bool value);
void config_set_int(config_t *config, const char *section, const char *name, long long value);
void config_set_uint(config_t *config, const char *section, const char *name, unsigned long long value);
int config_save(config_t *config);
}

namespace {

constexpr const char *FILTER_ID = "camera_tally_rec_trigger_filter";
constexpr const char *DOCK_ID = "recpilot-dock";
constexpr const char *APP_NAME = "RecPilot ✈";
constexpr int PREVIEW_EDGE_SIZE_LOCAL = 10;
constexpr double METADATA_CANVAS_WIDTH = 1920.0;
constexpr double METADATA_CANVAS_HEIGHT = 1080.0;
constexpr double METADATA_STANDARD_WIDTH = 0.07;
constexpr double METADATA_STANDARD_HEIGHT = 0.04;

enum class PickMode {
	None,
	DetectionCenter,
	ClipName,
	MetadataField,
	Color,
};

enum class DockLanguage {
	English,
	French,
	Minion,
};

struct DetectionPreset {
	const char *name;
	double center_x;
	double center_y;
	double radius;
	double ocr_x;
	double ocr_y;
	double ocr_width;
	double ocr_height;
};

struct MetadataFieldRow {
	QFrame *frame = nullptr;
	QComboBox *name = nullptr;
	QLineEdit *value = nullptr;
	QPushButton *pickButton = nullptr;
	QPushButton *removeButton = nullptr;
	QToolButton *detailsToggle = nullptr;
	QWidget *detailsPanel = nullptr;
	QDoubleSpinBox *x = nullptr;
	QDoubleSpinBox *y = nullptr;
	QDoubleSpinBox *width = nullptr;
	QDoubleSpinBox *height = nullptr;
};

constexpr std::array<DetectionPreset, 6> BUILTIN_PRESETS = {{
	{"ARRI ALEXA 35 - bottom status", 0.500, 0.978, 0.014, 0.335, 0.952, 0.140, 0.045},
	{"ARRI ALEXA 35 - bottom status wide", 0.500, 0.978, 0.014, 0.315, 0.948, 0.185, 0.050},
	{"Sony VENICE - bottom status", 0.430, 0.975, 0.014, 0.550, 0.945, 0.135, 0.055},
	{"RED V-RAPTOR - top REC dot", 0.985, 0.025, 0.018, 0.000, 0.940, 0.135, 0.055},
	{"RED V-RAPTOR - top REC dot wide name", 0.985, 0.025, 0.018, 0.000, 0.925, 0.170, 0.070},
	{"Phantom Flex 4K - top left REC", 0.035, 0.065, 0.026, 0.005, 0.820, 0.110, 0.070},
}};

class RecTriggerDock final : public QWidget {
public:
	RecTriggerDock()
	{
		setObjectName("RecTriggerDock");
		setMinimumWidth(220);
		setStyleSheet(
			"#RecTriggerDock { background: #17191f; color: #e8eaf0; }"
			"QLabel#Status { color: #9aa3b2; }"
			"QLabel#AppTitle { color: #f2f3f5; font-size: 18px; font-weight: 800; }"
			"QLabel#TopButtonLabel { color: #c7ceda; font-size: 11px; font-weight: 700; }"
			"QLabel#ClipNameStatus { color: #ffffff; background: #101218; border: 1px solid #394050;"
			"border-radius: 6px; padding: 9px; font-size: 16px; font-weight: 700; }"
			"QFrame#Panel { background: #20232b; border: 1px solid #303542; border-radius: 8px; }"
			"QCheckBox { spacing: 8px; }"
			"QPushButton { background: #343a46; border: 1px solid #454c5c; border-radius: 6px; padding: 6px 10px; }"
			"QPushButton:hover { background: #3f4654; }"
			"QPushButton#TopToggle { min-width: 44px; max-width: 44px; min-height: 44px; max-height: 44px;"
			"border-radius: 10px; font-size: 20px; font-weight: 800; }"
			"QPushButton#TopToggle[active=\"true\"] { background: #2f704d; border: 2px solid #6be69a; color: #ffffff; }"
			"QPushButton#TopToggle[active=\"false\"] { background: #303642; border: 2px solid #6a7280; color: #d8dde8; }"
			"QPushButton#RecButton { min-width: 44px; max-width: 44px; min-height: 44px; max-height: 44px;"
			"border-radius: 22px; font-size: 20px; font-weight: 800; }"
			"QPushButton#RecButton[recording=\"true\"] { background: #e62626; border: 2px solid #ffb3b3; color: #ffffff; }"
			"QPushButton#RecButton[recording=\"false\"] { background: #303642; border: 2px solid #6a7280; color: #f2f3f5; }"
			"QDoubleSpinBox, QSpinBox { background: #101218; border: 1px solid #394050; border-radius: 5px; padding: 3px; }"
			"QSlider::groove:horizontal { height: 5px; background: #363c48; border-radius: 2px; }"
			"QSlider::handle:horizontal { width: 14px; margin: -5px 0; border-radius: 7px; background: #ff4d3d; }");

		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(10, 10, 10, 10);
		root->setSpacing(8);

		auto *titleRow = new QHBoxLayout();
		auto *title = new QLabel(APP_NAME);
		title->setObjectName("AppTitle");
		titleRow->addWidget(title);
		titleRow->addStretch();
		root->addLayout(titleRow);

		auto *header = new QHBoxLayout();
		header->setSpacing(12);
		header->addStretch();
		recButton = new QPushButton();
		recButton->setObjectName("RecButton");
		recButtonLabel = createTopButtonLabel("Rec");
		header->addLayout(createTopButtonColumn(recButton, recButtonLabel));
		armedButton = createTopToggleButton("✓", "Armed");
		armedButtonLabel = createTopButtonLabel("Armed");
		header->addLayout(createTopButtonColumn(armedButton, armedButtonLabel));
		showOverlayButton = createTopToggleButton("👁", "Show detection overlay");
		showOverlayButtonLabel = createTopButtonLabel("Overlay");
		header->addLayout(createTopButtonColumn(showOverlayButton, showOverlayButtonLabel));
		header->addStretch();
		root->addLayout(header);

		status = new QLabel("No filter selected");
		status->setObjectName("Status");
		status->setWordWrap(true);
		status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		status->setMinimumHeight(44);
		status->setMaximumHeight(44);
		root->addWidget(status);

		auto *panel = new QFrame();
		panel->setObjectName("Panel");
		auto *panelLayout = new QVBoxLayout(panel);
		panelLayout->setContentsMargins(10, 10, 10, 10);
		panelLayout->setSpacing(8);

		sectionPicker = new QComboBox();
		sectionPicker->addItem("General");
		sectionPicker->addItem("Detection Center");
		sectionPicker->addItem("Clip Name");
		sectionPicker->addItem("Presets");
		sectionPicker->addItem("Metadata");
		sectionPicker->addItem("Clapperboard");
		panelLayout->addWidget(sectionPicker);

		sections = new QStackedWidget();
		sections->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		panelLayout->addWidget(sections, 1);

		auto *generalPage = new QWidget();
		auto *generalLayout = new QVBoxLayout(generalPage);
		generalLayout->setContentsMargins(0, 0, 0, 0);
		generalLayout->setSpacing(8);
		ocrEnabled = new QCheckBox("OCR clip name");
		ocrEnabled->setChecked(true);
		ocrEnabled->hide();

		alwaysArmOnLaunch = new QCheckBox("Always armed on OBS launch");
		tagText(alwaysArmOnLaunch, "Always armed on OBS launch");
		generalLayout->addWidget(alwaysArmOnLaunch);

		auto *recordingFolderRow = new QGridLayout();
		recordingFolderRow->setColumnStretch(0, 1);
		auto *recordingFolderLabel = new QLabel("Recording folder");
		tagText(recordingFolderLabel, "Recording folder");
		recordingFolderPath = new QLineEdit();
		recordingFolderPath->setReadOnly(true);
		recordingFolderPath->setPlaceholderText("OBS default folder");
		recordingFolderButton = new QPushButton("Choose…");
		tagText(recordingFolderButton, "Choose...");
		recordingFolderRow->addWidget(recordingFolderLabel, 0, 0, 1, 2);
		recordingFolderRow->addWidget(recordingFolderPath, 1, 0);
		recordingFolderRow->addWidget(recordingFolderButton, 1, 1);
		generalLayout->addLayout(recordingFolderRow);

		autoFolderEnabled = new QCheckBox("Automatic folder");
		tagText(autoFolderEnabled, "Automatic folder");
		generalLayout->addWidget(autoFolderEnabled);
		auto *autoFolderRow = new QHBoxLayout();
		autoFolderRow->setContentsMargins(18, 0, 0, 0);
		autoFolderRow->setSpacing(16);
		autoFolderByDate = new QCheckBox("By date");
		autoFolderByCamera = new QCheckBox("By camera");
		autoFolderByCard = new QCheckBox("By card");
		tagText(autoFolderByDate, "By date");
		tagText(autoFolderByCamera, "By camera");
		tagText(autoFolderByCard, "By card");
		autoFolderRow->addWidget(autoFolderByDate);
		autoFolderRow->addWidget(autoFolderByCamera);
		autoFolderRow->addWidget(autoFolderByCard);
		autoFolderRow->addStretch();
		generalLayout->addLayout(autoFolderRow);

		addOneToClipName = new QCheckBox("Add +1 to Clipname (ARRI camera)");
		addOneToClipName->setChecked(false);
		tagText(addOneToClipName, "Add +1 to Clipname (ARRI camera)");
		generalLayout->addWidget(addOneToClipName);

		clapperboardSaveSnapshots = new QCheckBox("Save Clapperboard snapshots");
		clapperboardSaveSnapshots->setChecked(true);
		tagText(clapperboardSaveSnapshots, "Save Clapperboard snapshots");
		generalLayout->addWidget(clapperboardSaveSnapshots);

		auto *recordingCodecRow = new QGridLayout();
		auto *recordingCodecLabel = new QLabel("Recording codec");
		tagText(recordingCodecLabel, "Recording codec");
		recordingCodec = new QComboBox();
		recordingCodec->addItem("ProRes HQ", "prores_hq");
		recordingCodec->addItem("ProRes LT", "prores_lt");
		recordingCodec->addItem("H265", "h265");
		recordingCodecRow->addWidget(recordingCodecLabel, 0, 0);
		recordingCodecRow->addWidget(recordingCodec, 1, 0);
		generalLayout->addLayout(recordingCodecRow);

		auto *recordingResolutionRow = new QGridLayout();
		auto *recordingResolutionLabel = new QLabel("Recording resolution");
		tagText(recordingResolutionLabel, "Recording resolution");
		recordingResolution = new QComboBox();
		recordingResolution->addItem("Same as canvas", "same_canvas");
		recordingResolution->addItem("720p (1280x720)", "1280x720");
		recordingResolution->addItem("SD (720x576)", "720x576");
		recordingResolution->addItem("HD (1920x1080)", "1920x1080");
		recordingResolution->addItem("2K (2048x1080)", "2048x1080");
		recordingResolution->addItem("UHD (3840x2160)", "3840x2160");
		recordingResolution->addItem("4K DCI (4096x2160)", "4096x2160");
		recordingResolutionRow->addWidget(recordingResolutionLabel, 0, 0);
		recordingResolutionRow->addWidget(recordingResolution, 1, 0);
		generalLayout->addLayout(recordingResolutionRow);

		generalLayout->addStretch();
		sections->addWidget(generalPage);

		auto *detectionPage = new QWidget();
		auto *detectionLayout = new QVBoxLayout(detectionPage);
		detectionLayout->setContentsMargins(0, 0, 0, 0);
		detectionLayout->setSpacing(8);
		auto *colorRow = new QGridLayout();
		colorRow->setColumnStretch(1, 1);
		auto *colorLabel = new QLabel("Color selection");
		tagText(colorLabel, "Color selection");
		colorPreset = new QComboBox();
		addColorPreset("Red", 255, 0, 0);
		addColorPreset("Green", 0, 210, 65);
		addColorPreset("Blue", 0, 120, 255);
		addColorPreset("Yellow", 255, 214, 0);
		addColorPreset("Orange", 255, 128, 0);
		addColorPreset("Magenta", 255, 0, 190);
		addColorPreset("Cyan", 0, 210, 255);
		addColorPreset("White", 245, 245, 245);
		addColorPreset("Custom", -1, -1, -1);
		colorSwatch = new QPushButton();
		colorSwatch->setFixedSize(34, 28);
		colorSwatch->setToolTip("Choose custom detection color");
		pickColorButton = new QPushButton("  Pick from preview");
		tagText(pickColorButton, "Pick from preview");
		pickColorButton->setIcon(cursorIcon());
		pickColorButton->setIconSize(QSize(22, 22));
		pickColorButton->setToolTip("Click, then choose a color directly in the main OBS preview");
		colorRow->addWidget(colorLabel, 0, 0, 1, 3);
		colorRow->addWidget(colorPreset, 1, 0);
		colorRow->addWidget(colorSwatch, 1, 1);
		colorRow->addWidget(pickColorButton, 1, 2);
		detectionLayout->addLayout(colorRow);
		fineTuneToggle = new QPushButton("▸ Fine tuning");
		fineTuneToggle->setCheckable(true);
		fineTuneToggle->setChecked(false);
		tagText(fineTuneToggle, "Fine tuning");
		detectionLayout->addWidget(fineTuneToggle);

		fineTunePanel = new QWidget();
		fineTunePanel->setVisible(false);
		auto *fineTuneLayout = new QVBoxLayout(fineTunePanel);
		fineTuneLayout->setContentsMargins(8, 10, 8, 8);
		fineTuneLayout->setSpacing(8);
		addDoubleControl(fineTuneLayout, "Color dominance", redThreshold, "red_threshold", 0.25, 0.9, 0.01, 2);
		addDoubleControl(fineTuneLayout, "Minimum color coverage", redCoverage, "red_coverage", 0.001, 0.2,
				 0.001, 3);
		addIntControl(fineTuneLayout, "Color detected frames before start", startFrames, "start_frames", 1, 30);
		addIntControl(fineTuneLayout, "Color lost frames before stop", stopFrames, "stop_frames", 1, 120);
		detectionLayout->addWidget(fineTunePanel);
		autoDetectCenterButton = new QPushButton("Auto detect circle");
		tagText(autoDetectCenterButton, "Auto detect circle");
		autoDetectCenterButton->setToolTip("Analyze the current image and place detection on the selected color circle");
		detectionLayout->addWidget(autoDetectCenterButton);
		selectCenterButton = new QPushButton("  Select in one click");
		tagText(selectCenterButton, "Select in one click");
		selectCenterButton->setIcon(cursorIcon());
		selectCenterButton->setIconSize(QSize(22, 22));
		selectCenterButton->setToolTip("Click, then choose the detection point in the main OBS preview");
		detectionLayout->addWidget(selectCenterButton);
		addDoubleControl(detectionLayout, "Detection center X", centerX, "center_x", 0.0, 100.0, 0.1, 1);
		addDoubleControl(detectionLayout, "Detection center Y", centerY, "center_y", 0.0, 100.0, 0.1, 1);
		addDoubleControl(detectionLayout, "Detection radius", radius, "radius", 0.0, 100.0, 1.0, 0);
		detectionLayout->addStretch();
		auto *detectionScroll = new QScrollArea();
		detectionScroll->setWidgetResizable(true);
		detectionScroll->setFrameShape(QFrame::NoFrame);
		detectionScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		detectionScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
		detectionScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
		detectionScroll->setMinimumHeight(0);
		detectionScroll->setWidget(detectionPage);
		sections->addWidget(detectionScroll);

		auto *clipPage = new QWidget();
		auto *clipLayout = new QVBoxLayout(clipPage);
		clipLayout->setContentsMargins(0, 0, 0, 0);
		clipLayout->setSpacing(8);
		ocrOToZero = new QCheckBox("Replace O with 0");
		ocrSpacesToUnderscores = new QCheckBox("Replace spaces with underscores");
		ocrRemoveSpaces = new QCheckBox("Remove spaces");
		tagText(ocrOToZero, "Replace O with 0");
		tagText(ocrSpacesToUnderscores, "Replace spaces with underscores");
		tagText(ocrRemoveSpaces, "Remove spaces");
		clipLayout->addWidget(ocrOToZero);
		clipLayout->addWidget(ocrSpacesToUnderscores);
		clipLayout->addWidget(ocrRemoveSpaces);
		autoDetectClipNameButton = new QPushButton("Auto detect clip name");
		tagText(autoDetectClipNameButton, "Auto detect clip name");
		autoDetectClipNameButton->setToolTip("Analyze the current image and place the clip name OCR box");
		clipLayout->addWidget(autoDetectClipNameButton);
		selectClipNameButton = new QPushButton("  Select in one click");
		tagText(selectClipNameButton, "Select in one click");
		selectClipNameButton->setIcon(cursorIcon());
		selectClipNameButton->setIconSize(QSize(22, 22));
		selectClipNameButton->setToolTip("Click, then choose the center of the clip name text in the main OBS preview");
		clipLayout->addWidget(selectClipNameButton);
			addDoubleControl(clipLayout, "Clip name X", ocrX, "ocr_x", 0.0, 1.0, 0.001, 3);
			addDoubleControl(clipLayout, "Clip name Y", ocrY, "ocr_y", 0.0, 1.0, 0.001, 3);
		addDoubleControl(clipLayout, "Clip name width", ocrWidth, "ocr_width", 0.0, 100.0, 1.0, 0);
		addDoubleControl(clipLayout, "Clip name height", ocrHeight, "ocr_height", 0.0, 100.0, 1.0, 0);
		clipLayout->addStretch();
		sections->addWidget(clipPage);

		auto *presetPage = new QWidget();
		auto *presetLayout = new QVBoxLayout(presetPage);
		presetLayout->setContentsMargins(0, 0, 0, 0);
		presetLayout->setSpacing(8);
		presetPicker = new QComboBox();
		presetLayout->addWidget(presetPicker);
		auto *presetButtons = new QHBoxLayout();
		loadPresetButton = new QPushButton("Load");
		savePresetButton = new QPushButton("Save");
		deletePresetButton = new QPushButton("Delete");
		tagText(loadPresetButton, "Load");
		tagText(savePresetButton, "Save");
		tagText(deletePresetButton, "Delete");
		presetButtons->addWidget(loadPresetButton);
		presetButtons->addWidget(savePresetButton);
		presetButtons->addWidget(deletePresetButton);
		presetLayout->addLayout(presetButtons);
		presetHint = new QLabel("Layouts include Detection Center, Clip Name, and metadata fields.");
		tagText(presetHint, "Layouts include Detection Center, Clip Name, and metadata fields.");
		presetHint->setObjectName("Status");
		presetHint->setWordWrap(true);
		presetLayout->addWidget(presetHint);
		presetLayout->addStretch();
		sections->addWidget(presetPage);

		auto *metadataPage = new QWidget();
		auto *metadataLayout = new QVBoxLayout(metadataPage);
		metadataLayout->setContentsMargins(0, 0, 0, 0);
		metadataLayout->setSpacing(8);
			metadataCsvPerCard = new QCheckBox("ZoeLog CSV per card");
			tagText(metadataCsvPerCard, "ZoeLog CSV per card");
			metadataLayout->addWidget(metadataCsvPerCard);
		metadataAutoDetectButton = new QPushButton("Auto-detect metadata");
		tagText(metadataAutoDetectButton, "Auto-detect metadata");
		metadataLayout->addWidget(metadataAutoDetectButton);
		metadataFieldsContainer = new QWidget();
		metadataFieldsLayout = new QVBoxLayout(metadataFieldsContainer);
		metadataFieldsLayout->setContentsMargins(0, 0, 0, 0);
		metadataFieldsLayout->setSpacing(8);
		metadataFieldsLayout->addStretch();
		auto *metadataScroll = new QScrollArea();
		metadataScroll->setWidgetResizable(true);
		metadataScroll->setFrameShape(QFrame::NoFrame);
		metadataScroll->setWidget(metadataFieldsContainer);
		metadataLayout->addWidget(metadataScroll, 1);
		addMetadataFieldButton = new QPushButton("Add metadata field");
		tagText(addMetadataFieldButton, "Add metadata field");
		metadataLayout->addWidget(addMetadataFieldButton);
		sections->addWidget(metadataPage);

		auto *clapperboardPage = new QWidget();
		auto *clapperboardLayout = new QVBoxLayout(clapperboardPage);
		clapperboardLayout->setContentsMargins(0, 0, 0, 0);
		clapperboardLayout->setSpacing(8);
		auto *clapperboardPreviewRow = new QHBoxLayout();
		clapperboardPreviewRow->setContentsMargins(0, 0, 0, 0);
		clapperboardPreviewRow->setSpacing(8);
		clapperboardPreview = new QLabel("No clap image yet");
		clapperboardPreview->setAlignment(Qt::AlignCenter);
		clapperboardPreview->setFixedHeight(190);
		clapperboardPreview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		clapperboardPreview->setMouseTracking(true);
		clapperboardPreview->installEventFilter(this);
		clapperboardPreview->setStyleSheet(
			"background: #101218; border: 1px solid #394050; border-radius: 9px; color: #9aa3b2;");
		tagText(clapperboardPreview, "No clap image yet");
		clapperboardPreviewRow->addWidget(clapperboardPreview, 1);

		auto *snapshotControls = new QVBoxLayout();
		snapshotControls->setContentsMargins(0, 0, 0, 0);
		snapshotControls->setSpacing(6);
		clapperboardSnapshotButton = new QPushButton("1");
		clapperboardSnapshotButton->setFixedSize(34, 30);
		clapperboardSnapshotButton->setEnabled(false);
		snapshotControls->addWidget(clapperboardSnapshotButton);
		connect(clapperboardSnapshotButton, &QPushButton::clicked, this, [this]() { cycleClapperboardSnapshot(); });
		clapperboardZoomButton = new QPushButton();
		clapperboardZoomButton->setIcon(fitIcon(false));
		clapperboardZoomButton->setFixedSize(34, 30);
		clapperboardZoomButton->setToolTip("Toggle full view");
		snapshotControls->addWidget(clapperboardZoomButton);
		connect(clapperboardZoomButton, &QPushButton::clicked, this, [this]() {
			clapperboardZoomToClap = !clapperboardZoomToClap;
			clapperboardZoomButton->setIcon(fitIcon(!clapperboardZoomToClap));
			updateClapperboardPreviewPixmap();
		});
		clapperboardRotateButton = new QPushButton();
		clapperboardRotateButton->setIcon(rotateIcon());
		clapperboardRotateButton->setFixedSize(34, 30);
		clapperboardRotateButton->setToolTip("Rotate preview");
		snapshotControls->addWidget(clapperboardRotateButton);
		connect(clapperboardRotateButton, &QPushButton::clicked, this, [this]() {
			clapperboardRotation = (clapperboardRotation + 90) % 360;
			updateClapperboardPreviewPixmap();
		});
		snapshotControls->addStretch();
		clapperboardPreviewRow->addLayout(snapshotControls);
		clapperboardLayout->addLayout(clapperboardPreviewRow);

		clapperboardLoupe = new QLabel(this, Qt::ToolTip);
		clapperboardLoupe->setObjectName("ClapperboardLoupe");
		clapperboardLoupe->setAlignment(Qt::AlignCenter);
		clapperboardLoupe->setFixedSize(460, 300);
		clapperboardLoupe->setStyleSheet("background: #101218; border: 2px solid #70e58a; border-radius: 9px;");
		clapperboardLoupe->hide();

		auto *clapHint = new QLabel("Clapperboard OCR fills these fields from the current image.");
		clapHint->setObjectName("Status");
		clapHint->setWordWrap(true);
		tagText(clapHint, "Clapperboard OCR fills these fields from the current image.");
		clapperboardLayout->addWidget(clapHint);
		auto *clapScroll = new QScrollArea();
		clapScroll->setWidgetResizable(true);
		clapScroll->setFrameShape(QFrame::NoFrame);
		auto *clapContainer = new QWidget();
		clapperboardFieldsLayout = new QGridLayout(clapContainer);
		clapperboardFieldsLayout->setContentsMargins(0, 0, 0, 0);
		clapperboardFieldsLayout->setHorizontalSpacing(8);
		clapperboardFieldsLayout->setVerticalSpacing(8);
		for (const QString &field : clapperboardFieldSuggestions())
			addClapperboardPageFieldRow(field);
		clapScroll->setWidget(clapContainer);
		clapperboardLayout->addWidget(clapScroll, 1);
		sections->addWidget(clapperboardPage);

			root->addWidget(panel, 1);

			clipName = createClipNameLabel();
			root->addWidget(clipName);

			auto *dockFooter = new QHBoxLayout();
			dockFooter->setContentsMargins(0, 0, 0, 0);
			coffeeButton = new QPushButton();
			coffeeButton->setIcon(kofiIcon());
			coffeeButton->setIconSize(QSize(28, 22));
			coffeeButton->setText(" Ko-fi");
			coffeeButton->setToolTip("Support RecPilot");
			tagText(coffeeButton, "Ko-fi");
			dockFooter->addWidget(coffeeButton, 0, Qt::AlignLeft);
			dockFooter->addStretch();
			clapperboardButton = new QPushButton();
			clapperboardButton->setIcon(clapIcon());
			clapperboardButton->setIconSize(QSize(24, 24));
			clapperboardButton->setToolTip("Clapperboard");
			dockFooter->addWidget(clapperboardButton, 0, Qt::AlignCenter);
			dockFooter->addStretch();
			languageButton = new QPushButton("🇬🇧 English");
			tagText(languageButton, "Language");
			dockFooter->addWidget(languageButton, 0, Qt::AlignRight);
			root->addLayout(dockFooter);

		connect(recButton, &QPushButton::clicked, this, [this]() { toggleRecording(); });
		connect(armedButton, &QPushButton::toggled, this, [this](bool value) {
			setBool("armed", value);
			updateTopToggleButtons();
		});
		connect(showOverlayButton, &QPushButton::toggled, this, [this](bool value) {
			setBool("show_overlay", value);
			updateTopToggleButtons();
		});
		connect(alwaysArmOnLaunch, &QCheckBox::toggled, this, [this](bool value) {
			updateSettings([value](obs_data_t *settings) {
				obs_data_set_bool(settings, "arm_on_launch", value);
				obs_data_set_bool(settings, "armed", value);
			});
			setBlocked(armedButton, value);
			updateTopToggleButtons();
		});
			connect(metadataCsvPerCard, &QCheckBox::toggled, this,
				[this](bool value) { setBool("metadata_csv_per_card", value); });
		connect(clapperboardSaveSnapshots, &QCheckBox::toggled, this,
			[this](bool value) { setBool("clapperboard_save_snapshots", value); });
		connect(metadataAutoDetectButton, &QPushButton::clicked, this, [this]() { requestMetadataAutoDetect(); });
		connect(addMetadataFieldButton, &QPushButton::clicked, this, [this]() {
			addMetadataFieldRow({});
			saveMetadataRowsToSettings();
		});
		connect(sectionPicker, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
			if (sections)
				sections->setCurrentIndex(index);
			updateMetadataOverlayVisibility();
		});
		connect(addOneToClipName, &QCheckBox::toggled, this,
			[this](bool value) { setBool("ocr_add_one", value); });
		connect(autoFolderEnabled, &QCheckBox::toggled, this, [this](bool value) {
			setBool("auto_folder_enabled", value);
			updateAutoFolderUi();
		});
		connect(autoFolderByDate, &QCheckBox::toggled, this,
			[this](bool value) { setBool("auto_folder_by_date", value); });
		connect(autoFolderByCamera, &QCheckBox::toggled, this,
			[this](bool value) { setBool("auto_folder_by_camera", value); });
		connect(autoFolderByCard, &QCheckBox::toggled, this,
			[this](bool value) { setBool("auto_folder_by_card", value); });
		connect(colorPreset, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			[this](int index) { applyColorPreset(index); });
		connect(colorSwatch, &QPushButton::clicked, this, [this]() { chooseCustomColor(); });
		connect(pickColorButton, &QPushButton::clicked, this, [this]() { beginColorSelection(); });
		connect(fineTuneToggle, &QPushButton::toggled, this, [this](bool expanded) {
			if (fineTunePanel)
				fineTunePanel->setVisible(expanded);
			updateFineTuneToggle();
		});
		connect(languageButton, &QPushButton::clicked, this, [this]() {
			if (language == DockLanguage::English)
				language = DockLanguage::French;
			else if (language == DockLanguage::French)
				language = DockLanguage::Minion;
			else
				language = DockLanguage::English;
			applyLanguage();
		});
		connect(coffeeButton, &QPushButton::clicked, this, [this]() { showCoffeeDialog(); });
		connect(clapperboardButton, &QPushButton::clicked, this, [this]() { requestClapperboard(); });
		connect(ocrOToZero, &QCheckBox::toggled, this, [this](bool value) { setBool("ocr_o_to_zero", value); });
		connect(ocrSpacesToUnderscores, &QCheckBox::toggled, this,
			[this](bool value) { setSpaceMode(value, ocrRemoveSpaces && ocrRemoveSpaces->isChecked()); });
		connect(ocrRemoveSpaces, &QCheckBox::toggled, this, [this](bool value) {
			setSpaceMode(ocrSpacesToUnderscores && ocrSpacesToUnderscores->isChecked(), value);
		});
		connect(recordingFolderButton, &QPushButton::clicked, this, [this]() { chooseRecordingFolder(); });
		connect(recordingCodec, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			[this](int index) { applyRecordingCodec(index); });
		connect(recordingResolution, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			[this](int index) { applyRecordingResolution(index); });
		connect(autoDetectCenterButton, &QPushButton::clicked, this, [this]() { requestAutoDetectCenter(); });
		connect(autoDetectClipNameButton, &QPushButton::clicked, this, [this]() { requestAutoDetectClipName(); });
		connect(selectCenterButton, &QPushButton::clicked, this, [this]() { beginCenterSelection(); });
		connect(selectClipNameButton, &QPushButton::clicked, this, [this]() { beginClipNameSelection(); });
		connect(loadPresetButton, &QPushButton::clicked, this, [this]() { loadSelectedPreset(); });
		connect(savePresetButton, &QPushButton::clicked, this, [this]() { saveCurrentPreset(); });
		connect(deletePresetButton, &QPushButton::clicked, this, [this]() { deleteSelectedPreset(); });

		reloadPresets();
		applyLanguage();

		pollTimer = new QTimer(this);
		connect(pollTimer, &QTimer::timeout, this, [this]() { refreshTarget(); });
		pollTimer->start(1500);

		coffeeReminderTimer = new QTimer(this);
		coffeeReminderTimer->setSingleShot(true);
		connect(coffeeReminderTimer, &QTimer::timeout, this, [this]() { showCoffeeDialog(true); });

		refreshTarget();
		QTimer::singleShot(1000, this, [this]() {
			recordingOutputUpdatesReady = true;
			refreshRecordingCodecAvailability();
			if (pendingRecordingCodecApply && recordingCodec) {
				pendingRecordingCodecApply = false;
				applyRecordingCodec(recordingCodec->currentIndex());
			}
		});
	}

	~RecTriggerDock() override
		{
			removePreviewPickEventFilters();
			hidePreviewPickHint();
			hidePreviewResultToast();
			if (clapperboardLoupe)
				clapperboardLoupe->hide();
			clearMetadataRows();
			if (filter)
				obs_source_release(filter);
		}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (watched == clapperboardPreview) {
			if (event->type() == QEvent::MouseButtonPress) {
				auto *mouseEvent = static_cast<QMouseEvent *>(event);
				if (mouseEvent->button() == Qt::LeftButton && beginClapperboardPan(mouseEvent->pos()))
					return true;
			}
			if (event->type() == QEvent::MouseMove) {
				auto *mouseEvent = static_cast<QMouseEvent *>(event);
				if (clapperboardPanning) {
					panClapperboardCrop(mouseEvent->pos());
					return true;
				}
				updateClapperboardLoupe(mouseEvent->pos());
				return false;
			}
			if (event->type() == QEvent::MouseButtonRelease) {
				auto *mouseEvent = static_cast<QMouseEvent *>(event);
				if (mouseEvent->button() == Qt::LeftButton && clapperboardPanning) {
					endClapperboardPan();
					return true;
				}
			}
			if (event->type() == QEvent::Leave) {
				endClapperboardPan();
				if (clapperboardLoupe)
					clapperboardLoupe->hide();
				return false;
			}
		}

		if (pickMode == PickMode::None || event->type() != QEvent::MouseButtonPress)
			return QWidget::eventFilter(watched, event);

		auto *mouseEvent = static_cast<QMouseEvent *>(event);
		if (mouseEvent->button() != Qt::LeftButton)
			return QWidget::eventFilter(watched, event);

		double x = 0.0;
		double y = 0.0;
		if (!mainPreviewPosition(mouseEvent, x, y)) {
			return false;
		}

		hidePreviewPickHint();
		if (pickMode == PickMode::DetectionCenter)
			setDetectionCenterFromCursor(x, y);
		else if (pickMode == PickMode::ClipName)
			setClipNameCenterFromCursor(x, y);
		else if (pickMode == PickMode::MetadataField)
			setMetadataFieldCenterFromCursor(x, y);
		else if (pickMode == PickMode::Color)
			setDetectionColorFromCursor(x, y);
		return true;
	}

	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		updateClapperboardPreviewPixmap();
	}

private:
	QLabel *status = nullptr;
	QPushButton *recButton = nullptr;
	QLabel *recButtonLabel = nullptr;
	QPushButton *armedButton = nullptr;
	QLabel *armedButtonLabel = nullptr;
	QPushButton *showOverlayButton = nullptr;
	QLabel *showOverlayButtonLabel = nullptr;
	QComboBox *sectionPicker = nullptr;
	QStackedWidget *sections = nullptr;
	QCheckBox *ocrEnabled = nullptr;
	QCheckBox *alwaysArmOnLaunch = nullptr;
	QCheckBox *addOneToClipName = nullptr;
	QComboBox *colorPreset = nullptr;
	QPushButton *colorSwatch = nullptr;
	QPushButton *pickColorButton = nullptr;
		QPushButton *fineTuneToggle = nullptr;
		QWidget *fineTunePanel = nullptr;
		QPushButton *coffeeButton = nullptr;
		QPushButton *clapperboardButton = nullptr;
		QPushButton *languageButton = nullptr;
	QCheckBox *ocrOToZero = nullptr;
	QCheckBox *ocrSpacesToUnderscores = nullptr;
	QCheckBox *ocrRemoveSpaces = nullptr;
	QDoubleSpinBox *centerX = nullptr;
	QDoubleSpinBox *centerY = nullptr;
	QDoubleSpinBox *radius = nullptr;
	std::unordered_map<QDoubleSpinBox *, QSlider *> doubleSliders;
	QPushButton *autoDetectCenterButton = nullptr;
	QPushButton *autoDetectClipNameButton = nullptr;
	QPushButton *selectCenterButton = nullptr;
	QPushButton *selectClipNameButton = nullptr;
	QDoubleSpinBox *ocrX = nullptr;
	QDoubleSpinBox *ocrY = nullptr;
	QDoubleSpinBox *ocrWidth = nullptr;
	QDoubleSpinBox *ocrHeight = nullptr;
	QDoubleSpinBox *redThreshold = nullptr;
	QDoubleSpinBox *redCoverage = nullptr;
	QSpinBox *startFrames = nullptr;
	QSpinBox *stopFrames = nullptr;
	QLineEdit *recordingFolderPath = nullptr;
	QPushButton *recordingFolderButton = nullptr;
	QCheckBox *autoFolderEnabled = nullptr;
	QCheckBox *autoFolderByDate = nullptr;
	QCheckBox *autoFolderByCamera = nullptr;
	QCheckBox *autoFolderByCard = nullptr;
	QCheckBox *clapperboardSaveSnapshots = nullptr;
		QCheckBox *metadataCsvPerCard = nullptr;
	QPushButton *metadataAutoDetectButton = nullptr;
	QPushButton *addMetadataFieldButton = nullptr;
	QWidget *metadataFieldsContainer = nullptr;
	QVBoxLayout *metadataFieldsLayout = nullptr;
	std::vector<MetadataFieldRow *> metadataRows;
	QGridLayout *clapperboardFieldsLayout = nullptr;
	std::vector<std::pair<QString, QLineEdit *>> clapperboardRows;
	QLabel *clapperboardPreview = nullptr;
	QLabel *clapperboardLoupe = nullptr;
	QPixmap clapperboardPreviewPixmap;
	QPixmap clapperboardPreviewSourcePixmap;
	std::vector<QString> clapperboardSnapshotPaths;
	QPushButton *clapperboardSnapshotButton = nullptr;
	QPushButton *clapperboardZoomButton = nullptr;
	QPushButton *clapperboardRotateButton = nullptr;
	QRectF clapperboardCrop;
	QPoint clapperboardPanLastPos;
	int clapperboardSnapshotIndex = 0;
	int clapperboardRotation = 0;
	bool clapperboardZoomToClap = true;
	bool clapperboardPanning = false;
	MetadataFieldRow *activeMetadataPickRow = nullptr;
	bool metadataUiLoading = false;
	QString loadedMetadataJson;
	QComboBox *recordingCodec = nullptr;
	QComboBox *recordingResolution = nullptr;
	QLabel *clipName = nullptr;
	QComboBox *presetPicker = nullptr;
	QPushButton *loadPresetButton = nullptr;
	QPushButton *savePresetButton = nullptr;
		QPushButton *deletePresetButton = nullptr;
		QLabel *presetHint = nullptr;
		QLabel *previewPickHint = nullptr;
		QLabel *previewResultToast = nullptr;
		QString previewResultToastKey;
		bool previewResultToastSuccess = false;
		QString lastPickDebug;
	std::vector<QPointer<QObject>> previewPickEventTargets;
		QTimer *pollTimer = nullptr;
		QTimer *coffeeReminderTimer = nullptr;
		obs_source_t *filter = nullptr;
	QJsonArray customPresets;
	PickMode pickMode = PickMode::None;
	DockLanguage language = DockLanguage::English;
	bool recordingOutputUpdatesReady = false;
		bool pendingRecordingCodecApply = false;
		bool coffeeDialogVisible = false;
		QString temporaryStatus;
	int temporaryStatusRefreshes = 0;

	static void filterEnum(obs_source_t *, obs_source_t *child, void *param)
	{
		auto **found = static_cast<obs_source_t **>(param);
		if (*found)
			return;
		const char *id = obs_source_get_unversioned_id(child);
		if (id && strcmp(id, FILTER_ID) == 0)
			*found = obs_source_get_ref(child);
	}

	static bool sceneItemEnum(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		auto **found = static_cast<obs_source_t **>(param);
		if (*found)
			return false;

		obs_source_t *source = obs_sceneitem_get_source(item);
		if (source)
			obs_source_enum_filters(source, filterEnum, found);

		return *found == nullptr;
	}

	struct FilterSceneItemSearch {
		obs_source_t *filter = nullptr;
		obs_sceneitem_t *item = nullptr;
	};

	static void filterItemEnum(obs_source_t *, obs_source_t *child, void *param)
	{
		auto *search = static_cast<FilterSceneItemSearch *>(param);
		if (!search->item && child == search->filter)
			search->item = reinterpret_cast<obs_sceneitem_t *>(1);
	}

	static bool sceneItemForFilterEnum(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		auto *search = static_cast<FilterSceneItemSearch *>(param);
		if (search->item)
			return false;

		if (obs_sceneitem_is_group(item)) {
			obs_sceneitem_group_enum_items(item, sceneItemForFilterEnum, param);
			return search->item == nullptr;
		}

		obs_source_t *source = obs_sceneitem_get_source(item);
		if (!source)
			return true;

		FilterSceneItemSearch localSearch = *search;
		obs_source_enum_filters(source, filterItemEnum, &localSearch);
		if (localSearch.item)
			search->item = item;

		return search->item == nullptr;
	}

	static obs_source_t *findFilterInCurrentScene()
	{
		obs_source_t *sceneSource = obs_frontend_get_current_scene();
		if (!sceneSource)
			return nullptr;

		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		obs_source_t *found = nullptr;
		if (scene)
			obs_scene_enum_items(scene, sceneItemEnum, &found);

		obs_source_release(sceneSource);
		return found;
	}

	obs_sceneitem_t *findFilterSceneItem() const
	{
		if (!filter)
			return nullptr;

		obs_source_t *sceneSource = obs_frontend_get_current_scene();
		if (!sceneSource)
			return nullptr;

		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		FilterSceneItemSearch search;
		search.filter = filter;
		if (scene)
			obs_scene_enum_items(scene, sceneItemForFilterEnum, &search);

		obs_source_release(sceneSource);
		return search.item;
	}

	void refreshTarget()
	{
		obs_source_t *found = findFilterInCurrentScene();
		if (found != filter) {
			if (filter)
				obs_source_release(filter);
			filter = found;
		} else if (found) {
			obs_source_release(found);
		}

		loadValues();
	}

	void loadValues()
	{
		const bool enabled = filter != nullptr;
		for (auto *widget : findChildren<QWidget *>())
			widget->setEnabled(enabled || widget == recButton);

		updateRecButton();
		loadRecordingFolder();
		loadRecordingCodec();
		loadRecordingResolution();

		if (!filter) {
			status->setText(QString("Add %1 to a source in the current scene.").arg(APP_NAME));
			setClipNameText({});
			if (recordingFolderPath)
				recordingFolderPath->setEnabled(true);
			if (recordingFolderButton)
				recordingFolderButton->setEnabled(true);
			if (autoFolderEnabled)
				autoFolderEnabled->setEnabled(false);
			if (autoFolderByDate)
				autoFolderByDate->setEnabled(false);
			if (autoFolderByCamera)
				autoFolderByCamera->setEnabled(false);
			if (autoFolderByCard)
				autoFolderByCard->setEnabled(false);
			if (recordingCodec)
				recordingCodec->setEnabled(true);
			if (recordingResolution)
				recordingResolution->setEnabled(true);
			clearMetadataRows();
			return;
		}

		const char *name = obs_source_get_name(filter);
		setDefaultStatus(QString("Controlling: %1").arg(name ? name : APP_NAME));

		obs_data_t *settings = obs_source_get_settings(filter);
		if (!settings)
			return;

		setBlocked(armedButton, obs_data_get_bool(settings, "armed"));
		setBlocked(showOverlayButton, obs_data_get_bool(settings, "show_overlay"));
		updateTopToggleButtons();
		obs_data_set_bool(settings, "start_on_red", true);
		setBlocked(alwaysArmOnLaunch, obs_data_get_bool(settings, "arm_on_launch"));
		setBlocked(addOneToClipName, obs_data_get_bool(settings, "ocr_add_one"));
		setBlocked(autoFolderEnabled, obs_data_get_bool(settings, "auto_folder_enabled"));
		setBlocked(autoFolderByDate, obs_data_get_bool(settings, "auto_folder_by_date"));
		setBlocked(autoFolderByCamera, obs_data_get_bool(settings, "auto_folder_by_camera"));
		setBlocked(autoFolderByCard, obs_data_get_bool(settings, "auto_folder_by_card"));
		setBlocked(clapperboardSaveSnapshots, obs_data_get_bool(settings, "clapperboard_save_snapshots"));
			setBlocked(metadataCsvPerCard, obs_data_get_bool(settings, "metadata_csv_per_card"));
		loadMetadataRowsFromSettings(settings);
		const bool metadataVisible = isMetadataSectionActive();
		if (obs_data_get_bool(settings, "metadata_overlay_visible") != metadataVisible) {
			obs_data_set_bool(settings, "metadata_overlay_visible", metadataVisible);
			obs_source_update(filter, settings);
		}
		updateAutoFolderUi();
		setBlocked(redThreshold, obs_data_get_double(settings, "red_threshold"));
		setBlocked(redCoverage, obs_data_get_double(settings, "red_coverage"));
		setBlocked(startFrames, static_cast<int>(obs_data_get_int(settings, "start_frames")));
		setBlocked(stopFrames, static_cast<int>(obs_data_get_int(settings, "stop_frames")));
		int colorR = static_cast<int>(obs_data_get_int(settings, "color_r"));
		int colorG = static_cast<int>(obs_data_get_int(settings, "color_g"));
		int colorB = static_cast<int>(obs_data_get_int(settings, "color_b"));
		if (colorR == 0 && colorG == 0 && colorB == 0)
			colorR = 255;
			updateColorUi(colorR, colorG, colorB);
			if (obs_data_get_bool(settings, "coffee_thanks_confirmed")) {
				if (coffeeReminderTimer)
					coffeeReminderTimer->stop();
			} else {
				scheduleCoffeeReminder(false);
			}
			setBlocked(ocrEnabled, obs_data_get_bool(settings, "ocr_enabled"));
		setBlocked(ocrOToZero, obs_data_get_bool(settings, "ocr_o_to_zero"));
		bool spacesToUnderscores = obs_data_get_bool(settings, "ocr_spaces_to_underscores");
		bool removeSpaces = obs_data_get_bool(settings, "ocr_remove_spaces");
		if (spacesToUnderscores && removeSpaces)
			removeSpaces = false;
		setBlocked(ocrSpacesToUnderscores, spacesToUnderscores);
		setBlocked(ocrRemoveSpaces, removeSpaces);
		setBlocked(centerX, displayValueForDoubleKey("center_x", obs_data_get_double(settings, "center_x")));
		setBlocked(centerY, displayValueForDoubleKey("center_y", obs_data_get_double(settings, "center_y")));
		setBlocked(radius, displayValueForDoubleKey("radius", obs_data_get_double(settings, "radius")));
		double storedOcrWidth = std::clamp(obs_data_get_double(settings, "ocr_width"), 0.0, clipNameWidthMax());
		double storedOcrHeight = std::clamp(obs_data_get_double(settings, "ocr_height"), 0.0, clipNameHeightMax());
		if (storedOcrWidth != obs_data_get_double(settings, "ocr_width") ||
		    storedOcrHeight != obs_data_get_double(settings, "ocr_height")) {
			obs_data_set_double(settings, "ocr_width", storedOcrWidth);
			obs_data_set_double(settings, "ocr_height", storedOcrHeight);
			obs_source_update(filter, settings);
		}
		setBlocked(ocrX, obs_data_get_double(settings, "ocr_x"));
		setBlocked(ocrY, obs_data_get_double(settings, "ocr_y"));
		setBlocked(ocrWidth, displayValueForDoubleKey("ocr_width", storedOcrWidth));
		setBlocked(ocrHeight, displayValueForDoubleKey("ocr_height", storedOcrHeight));
		syncDoubleSliders();

		const std::string currentClip = camera_tally_filter_get_clip_name(filter);
		setClipNameText(currentClip);
		readAutoDetectResult(settings);
		readAutoDetectClipNameResult(settings);
		readClapperboardResult(settings);

		obs_data_release(settings);
	}

	void setDefaultStatus(const QString &text)
	{
		if (!temporaryStatus.isEmpty() && temporaryStatusRefreshes > 0) {
			status->setText(temporaryStatus);
			status->setToolTip(temporaryStatus);
			temporaryStatusRefreshes--;
			if (temporaryStatusRefreshes <= 0)
				temporaryStatus.clear();
			return;
		}

		status->setText(text);
		status->setToolTip(text);
	}

	void setTemporaryStatus(const QString &text, int refreshes = 4)
	{
		temporaryStatus = text;
		temporaryStatusRefreshes = refreshes;
		status->setText(text);
		status->setToolTip(text);
	}

	void readAutoDetectResult(obs_data_t *settings)
	{
		if (!settings)
			return;

		const char *raw = obs_data_get_string(settings, "auto_detect_center_result");
			const QString result = raw ? QString::fromUtf8(raw) : QString();
			if (result.isEmpty() || result == "waiting")
				return;

			const char *detailRaw = obs_data_get_string(settings, "auto_detect_center_detail");
			const QString detail = detailRaw ? QString::fromUtf8(detailRaw).trimmed() : QString();
			if (result == "found") {
				showPreviewResultToast("Center detected", true);
				setTemporaryStatus(detail.isEmpty() ? trText("Auto detection found the circle.")
								    : QString("%1 %2").arg(trText("Auto detection found the circle."), detail),
						   5);
			} else if (result == "not_found") {
				showPreviewResultToast("Center not found", false);
				setTemporaryStatus(detail.isEmpty() ? trText("No matching circle found. Check the selected color.")
								    : QString("%1 %2").arg(
									      trText("No matching circle found."), detail),
						   8);
		} else if (result == "no_frame") {
			setTemporaryStatus(trText("No image received. Make sure the controlled source is visible."), 5);
		}

		obs_data_set_string(settings, "auto_detect_center_result", "");
		obs_data_set_string(settings, "auto_detect_center_detail", "");
		obs_source_update(filter, settings);
	}

	void readAutoDetectClipNameResult(obs_data_t *settings)
	{
		if (!settings)
			return;

		const char *raw = obs_data_get_string(settings, "auto_detect_clip_name_result");
		const QString result = raw ? QString::fromUtf8(raw) : QString();
		if (result.isEmpty() || result == "waiting")
			return;

		const char *detailRaw = obs_data_get_string(settings, "auto_detect_clip_name_detail");
		const QString detail = detailRaw ? QString::fromUtf8(detailRaw).trimmed() : QString();

		obs_data_set_string(settings, "auto_detect_clip_name_result", "");
		obs_data_set_string(settings, "auto_detect_clip_name_detail", "");
		obs_source_update(filter, settings);

		if (result == "found") {
			showPreviewResultToast("Clip name detected", true);
			setTemporaryStatus(detail.isEmpty() ? trText("Clip name detected.")
							    : QString("%1 %2").arg(trText("Clip name detected."), detail),
					   5);
		} else if (result == "not_found") {
			showPreviewResultToast("Clip name not found", false);
			setTemporaryStatus(detail.isEmpty() ? trText("Clip name not found.")
							    : QString("%1 %2").arg(trText("Clip name not found."), detail),
					   8);
		} else if (result == "no_frame") {
			setTemporaryStatus(trText("No image received. Make sure the controlled source is visible."), 5);
		}
	}

	void readClapperboardResult(obs_data_t *settings)
	{
		if (!settings)
			return;

		const char *raw = obs_data_get_string(settings, "clapperboard_result");
		const QString result = raw ? QString::fromUtf8(raw) : QString();
		if (result.isEmpty() || result == "waiting")
			return;

		const char *jsonRaw = obs_data_get_string(settings, "clapperboard_fields_json");
		const QByteArray json = jsonRaw ? QByteArray(jsonRaw) : QByteArray();
		const char *targetRaw = obs_data_get_string(settings, "clapperboard_target");
		const QString target = targetRaw ? QString::fromUtf8(targetRaw) : QString("clapperboard");
		const char *imageRaw = obs_data_get_string(settings, "clapperboard_image_path");
		const QString imagePath = imageRaw ? QString::fromUtf8(imageRaw).trimmed() : QString();
		const char *imagePathsRaw = obs_data_get_string(settings, "clapperboard_image_paths_json");
		const QByteArray imagePathsJson = imagePathsRaw ? QByteArray(imagePathsRaw) : QByteArray("[]");
		const char *cropRaw = obs_data_get_string(settings, "clapperboard_crop_json");
		const QByteArray cropJson = cropRaw ? QByteArray(cropRaw) : QByteArray();

		obs_data_set_string(settings, "clapperboard_result", "");
		obs_data_set_string(settings, "clapperboard_fields_json", "[]");
		obs_data_set_string(settings, "clapperboard_image_path", "");
		obs_data_set_string(settings, "clapperboard_image_paths_json", "[]");
		obs_data_set_string(settings, "clapperboard_crop_json", "");
		obs_source_update(filter, settings);

		if (target != "metadata") {
			if (sectionPicker)
				sectionPicker->setCurrentIndex(5);
			QStringList paths = parseClapperboardSnapshotPaths(imagePathsJson);
			if (paths.isEmpty() && !imagePath.isEmpty())
				paths.push_back(imagePath);
			setClapperboardCrop(cropJson);
			if (!paths.isEmpty())
				setClapperboardImages(paths);
		}

		if (result == "found") {
			const int count = target == "metadata" ? applyClapperboardFields(json) : applyClapperboardPageFields(json);
			if (count > 0) {
				setTemporaryStatus(
					QString("%1 %2")
						.arg(target == "metadata" ? trText("Auto-detect filled metadata fields.")
									  : trText("Clapperboard filled fields."))
						.arg(count),
					6);
			} else {
				setTemporaryStatus(target == "metadata" ? trText("Auto-detect found no usable metadata.")
									: trText("Clapperboard found no usable data."),
						   6);
			}
		} else if (result == "not_found") {
			setTemporaryStatus(target == "metadata" ? trText("Auto-detect found no usable metadata.")
								: trText("Clapperboard found no usable data."),
					   6);
		} else if (result == "no_frame") {
			setTemporaryStatus(trText("No image received. Make sure the controlled source is visible."), 5);
		}
	}

	void setClipNameText(const std::string &currentClip)
	{
		const QString text = QString("%1: %2").arg(trText("Clip name"), currentClip.empty() ? "-" : currentClip.c_str());
		if (clipName)
			clipName->setText(text);
	}

	static QLabel *createClipNameLabel()
	{
		auto *label = new QLabel("Clip name: -");
		label->setObjectName("ClipNameStatus");
		label->setWordWrap(true);
		label->setAlignment(Qt::AlignCenter);
		label->setMinimumHeight(42);
		return label;
	}

	static QStringList parseClapperboardSnapshotPaths(const QByteArray &json)
	{
		QStringList paths;
		const QJsonDocument doc = QJsonDocument::fromJson(json);
		if (!doc.isArray())
			return paths;
		for (const QJsonValue value : doc.array()) {
			const QString path = value.toString().trimmed();
			if (!path.isEmpty())
				paths.push_back(path);
		}
		return paths;
	}

	void setClapperboardCrop(const QByteArray &json)
	{
		clapperboardCrop = {};
		const QJsonDocument doc = QJsonDocument::fromJson(json);
		if (!doc.isObject())
			return;
		const QJsonObject object = doc.object();
		const double x = object.value("x").toDouble(-1.0);
		const double y = object.value("y").toDouble(-1.0);
		const double w = object.value("width").toDouble(0.0);
		const double h = object.value("height").toDouble(0.0);
		if (x >= 0.0 && y >= 0.0 && w > 0.01 && h > 0.01)
			clapperboardCrop = QRectF(x, y, w, h).intersected(QRectF(0.0, 0.0, 1.0, 1.0));
	}

	void setClapperboardImages(const QStringList &paths)
	{
		clapperboardSnapshotPaths.clear();
		for (const QString &path : paths)
			clapperboardSnapshotPaths.push_back(path);
		clapperboardSnapshotIndex = 0;
		clapperboardRotation = 0;
		if (clapperboardSnapshotButton) {
			clapperboardSnapshotButton->setText("1");
			clapperboardSnapshotButton->setEnabled(clapperboardSnapshotPaths.size() > 1);
		}
		if (!clapperboardSnapshotPaths.empty())
			setClapperboardImage(clapperboardSnapshotPaths.front());
	}

	void cycleClapperboardSnapshot()
	{
		if (clapperboardSnapshotPaths.empty())
			return;
		selectClapperboardSnapshot((clapperboardSnapshotIndex + 1) %
					   static_cast<int>(clapperboardSnapshotPaths.size()));
	}

	void selectClapperboardSnapshot(int index)
	{
		if (index < 0 || index >= static_cast<int>(clapperboardSnapshotPaths.size()))
			return;
		clapperboardSnapshotIndex = index;
		if (clapperboardSnapshotButton)
			clapperboardSnapshotButton->setText(QString::number(index + 1));
		setClapperboardImage(clapperboardSnapshotPaths[static_cast<size_t>(index)]);
	}

	void setClapperboardImage(const QString &path)
	{
		QPixmap pixmap(path);
		if (pixmap.isNull())
			return;

		clapperboardPreviewSourcePixmap = pixmap;
		updateClapperboardPreviewPixmap();
		if (clapperboardPreview)
			clapperboardPreview->setToolTip(path);
	}

	void updateClapperboardPreviewPixmap()
	{
		if (!clapperboardPreview || clapperboardPreviewSourcePixmap.isNull())
			return;

		QPixmap display = clapperboardPreviewSourcePixmap;
		if (clapperboardZoomToClap && !clapperboardCrop.isNull()) {
			QRect cropRect(
				static_cast<int>(clapperboardCrop.x() * clapperboardPreviewSourcePixmap.width()),
				static_cast<int>(clapperboardCrop.y() * clapperboardPreviewSourcePixmap.height()),
				static_cast<int>(clapperboardCrop.width() * clapperboardPreviewSourcePixmap.width()),
				static_cast<int>(clapperboardCrop.height() * clapperboardPreviewSourcePixmap.height()));
			cropRect = cropRect.intersected(clapperboardPreviewSourcePixmap.rect());
			if (cropRect.width() > 4 && cropRect.height() > 4)
				display = clapperboardPreviewSourcePixmap.copy(cropRect);
		}
		if (clapperboardRotation != 0)
			display = display.transformed(QTransform().rotate(clapperboardRotation), Qt::SmoothTransformation);

		clapperboardPreviewPixmap = display;
		clapperboardPreview->setPixmap(clapperboardPreviewPixmap.scaled(
			clapperboardPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}

	QRect clapperboardPreviewImageRect() const
	{
		if (!clapperboardPreview || clapperboardPreviewPixmap.isNull())
			return {};

		const QSize scaled =
			clapperboardPreviewPixmap.size().scaled(clapperboardPreview->size(), Qt::KeepAspectRatio);
		return QRect((clapperboardPreview->width() - scaled.width()) / 2,
			     (clapperboardPreview->height() - scaled.height()) / 2, scaled.width(), scaled.height());
	}

	bool beginClapperboardPan(const QPoint &position)
	{
		if (!clapperboardPreview || !clapperboardZoomToClap || clapperboardCrop.isNull() ||
		    clapperboardPreviewPixmap.isNull())
			return false;

		const QRect imageRect = clapperboardPreviewImageRect();
		if (!imageRect.contains(position))
			return false;

		clapperboardPanning = true;
		clapperboardPanLastPos = position;
		if (clapperboardLoupe)
			clapperboardLoupe->hide();
		clapperboardPreview->setCursor(Qt::ClosedHandCursor);
		return true;
	}

	void panClapperboardCrop(const QPoint &position)
	{
		if (!clapperboardPanning || clapperboardCrop.isNull())
			return;

		const QRect imageRect = clapperboardPreviewImageRect();
		if (imageRect.width() <= 0 || imageRect.height() <= 0)
			return;

		const QPoint delta = position - clapperboardPanLastPos;
		clapperboardPanLastPos = position;

		const double dx = -static_cast<double>(delta.x()) / imageRect.width() * clapperboardCrop.width();
		const double dy = -static_cast<double>(delta.y()) / imageRect.height() * clapperboardCrop.height();
		const double x = std::clamp(clapperboardCrop.x() + dx, 0.0, 1.0 - clapperboardCrop.width());
		const double y = std::clamp(clapperboardCrop.y() + dy, 0.0, 1.0 - clapperboardCrop.height());
		clapperboardCrop.moveTo(x, y);
		updateClapperboardPreviewPixmap();
	}

	void endClapperboardPan()
	{
		if (!clapperboardPanning)
			return;
		clapperboardPanning = false;
		if (clapperboardPreview)
			clapperboardPreview->unsetCursor();
	}

	void updateClapperboardLoupe(const QPoint &position)
	{
		if (!clapperboardPreview || !clapperboardLoupe || clapperboardPreviewPixmap.isNull())
			return;

		const QRect imageRect = clapperboardPreviewImageRect();
		if (!imageRect.contains(position) || imageRect.width() <= 0 || imageRect.height() <= 0) {
			clapperboardLoupe->hide();
			return;
		}

		const double sx = static_cast<double>(clapperboardPreviewPixmap.width()) / imageRect.width();
		const double sy = static_cast<double>(clapperboardPreviewPixmap.height()) / imageRect.height();
		const int sourceX = static_cast<int>((position.x() - imageRect.x()) * sx);
		const int sourceY = static_cast<int>((position.y() - imageRect.y()) * sy);
		const int cropWidth = std::max(240, clapperboardPreviewPixmap.width() * 2 / 7);
		const int cropHeight = std::max(160, clapperboardPreviewPixmap.height() * 2 / 7);
		QRect crop(sourceX - cropWidth / 2, sourceY - cropHeight / 2, cropWidth, cropHeight);
		crop.moveLeft(std::clamp(crop.left(), 0, std::max(0, clapperboardPreviewPixmap.width() - crop.width())));
		crop.moveTop(std::clamp(crop.top(), 0, std::max(0, clapperboardPreviewPixmap.height() - crop.height())));

		clapperboardLoupe->setPixmap(clapperboardPreviewPixmap.copy(crop).scaled(
			clapperboardLoupe->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
		QPoint target = QCursor::pos() - QPoint(clapperboardLoupe->width() + 18, clapperboardLoupe->height() / 2);
		if (QScreen *screen = QGuiApplication::screenAt(QCursor::pos())) {
			const QRect available = screen->availableGeometry();
			target.setX(std::clamp(target.x(), available.left(), available.right() - clapperboardLoupe->width()));
			target.setY(std::clamp(target.y(), available.top(), available.bottom() - clapperboardLoupe->height()));
		}
		clapperboardLoupe->move(target);
		clapperboardLoupe->show();
	}

		static QStringList metadataFieldSuggestions()
		{
			return {
				"Slate",       "Scene",       "Date",        "Camera",      "Roll",       "Take",
				"Clip",        "Circled",     "Lens",        "Filters",     "Stop",       "Focus",
				"Lens Height", "FPS",         "Shutter",     "Film Stock",  "Tilt",       "Description",
				"Notes",       "Color Temp",  "ISO",         "Time Code",   "Lut",        "Aspect Ratio",
				"Format",      "Resolution",  "Origin Date", "Take Origin",
			};
		}

	static QStringList clapperboardFieldSuggestions()
	{
		return {"Camera", "Scene", "Shot", "Take", "Sequence", "Slate"};
	}

	void addClapperboardPageFieldRow(const QString &name)
	{
		if (!clapperboardFieldsLayout)
			return;

		const int index = static_cast<int>(clapperboardRows.size());
		const int column = index % 4;
		const int rowIndex = (index / 4) * 2;
		auto *value = new QLineEdit();
		value->setReadOnly(false);
		value->setPlaceholderText("-");
		value->setAlignment(Qt::AlignCenter);
		value->setMinimumHeight(44);
		value->setStyleSheet("font-size: 22px; font-weight: 700;");
		auto *label = new QLabel(name);
		label->setAlignment(Qt::AlignCenter);
		label->setObjectName("Status");
		clapperboardFieldsLayout->addWidget(value, rowIndex, column);
		clapperboardFieldsLayout->addWidget(label, rowIndex + 1, column);
		clapperboardRows.emplace_back(name, value);
	}

	int applyClapperboardPageFields(const QByteArray &json)
	{
		const QJsonDocument doc = QJsonDocument::fromJson(json);
		if (!doc.isArray())
			return 0;

		int applied = 0;
		std::set<QString> touched;
		for (const QJsonValue value : doc.array()) {
			const QJsonObject object = value.toObject();
			const QString name = object.value("name").toString().trimmed();
			const QString fieldValue = object.value("value").toString().trimmed();
			if (name.isEmpty() || fieldValue.isEmpty())
				continue;
			for (auto &row : clapperboardRows) {
				if (row.first.compare(name, Qt::CaseInsensitive) != 0 || !row.second)
					continue;
				row.second->setText(fieldValue);
				touched.insert(row.first.toLower());
				applied++;
				break;
			}
		}

		if (touched.find("camera") == touched.end()) {
			const QString camera = currentCameraLetterFromClipName();
			if (!camera.isEmpty()) {
				for (auto &row : clapperboardRows) {
					if (row.first.compare("Camera", Qt::CaseInsensitive) == 0 && row.second) {
						row.second->setText(camera);
						applied++;
						break;
					}
				}
			}
		}
		return applied;
	}

	QString currentCameraLetterFromClipName() const
	{
		if (!filter)
			return {};
		const QString clip = QString::fromStdString(camera_tally_filter_get_clip_name(filter));
		for (const QChar ch : clip) {
			if (ch.isLetter())
				return ch.toUpper();
		}
		return {};
	}

	void clearMetadataRows()
	{
		activeMetadataPickRow = nullptr;
		for (MetadataFieldRow *row : metadataRows) {
			if (row && row->frame)
				row->frame->deleteLater();
			delete row;
		}
		metadataRows.clear();
	}

	QDoubleSpinBox *createMetadataSpin(double value, double min, double max)
	{
		auto *box = new QDoubleSpinBox();
		box->setRange(min, max);
		box->setDecimals(3);
		box->setSingleStep(0.001);
		box->setKeyboardTracking(false);
		box->setMinimumWidth(76);
		box->setValue(value);
		return box;
	}

	QDoubleSpinBox *createMetadataPositionSpin(double value, double max)
	{
		auto *box = new QDoubleSpinBox();
		box->setRange(0.0, max);
		box->setDecimals(1);
		box->setSingleStep(1.0);
		box->setKeyboardTracking(false);
		box->setMinimumWidth(76);
		box->setSuffix(" px");
		box->setValue(value);
		return box;
	}

	QDoubleSpinBox *createMetadataSizeSpin(double normalizedValue, double standardValue)
	{
		auto *box = new QDoubleSpinBox();
		box->setRange(1.0, 1000.0);
		box->setDecimals(1);
		box->setSingleStep(2.0);
		box->setKeyboardTracking(false);
		box->setMinimumWidth(76);
		box->setSuffix(" %");
		box->setValue((normalizedValue / standardValue) * 100.0);
		return box;
	}

	static double metadataDisplayToNormalized(double displayValue, double standardValue)
	{
		return std::clamp((displayValue / 100.0) * standardValue, 0.001, 1.0);
	}

	static double metadataCenterToTopLeft(double centerPixels, double canvasPixels, double normalizedSize)
	{
		const double center = std::clamp(centerPixels / canvasPixels, 0.0, 1.0);
		return std::clamp(center - normalizedSize * 0.5, 0.0, std::max(0.0, 1.0 - normalizedSize));
	}

	static double metadataTopLeftToCenter(double topLeft, double normalizedSize, double canvasPixels)
	{
		return std::clamp((topLeft + normalizedSize * 0.5) * canvasPixels, 0.0, canvasPixels);
	}

	void addMetadataFieldRow(const QJsonObject &object)
	{
		if (!metadataFieldsLayout)
			return;

		auto *row = new MetadataFieldRow();
		row->frame = new QFrame();
		row->frame->setObjectName("Panel");
		auto *layout = new QGridLayout(row->frame);
		layout->setContentsMargins(8, 8, 8, 8);
		layout->setSpacing(6);
		layout->setColumnStretch(0, 1);
		row->name = new QComboBox();
		row->name->setEditable(true);
		row->name->addItems(metadataFieldSuggestions());
		row->name->setCurrentText(object.value("name").toString());
		auto *completer = new QCompleter(metadataFieldSuggestions(), row->name);
		completer->setCaseSensitivity(Qt::CaseInsensitive);
		completer->setFilterMode(Qt::MatchContains);
		row->name->setCompleter(completer);
		row->value = new QLineEdit(object.value("value").toString());
		row->value->setPlaceholderText(trText("Manual value or OCR result"));
		row->pickButton = new QPushButton();
		row->pickButton->setIcon(cursorIcon());
		row->pickButton->setIconSize(QSize(18, 18));
		row->pickButton->setToolTip(trText("Select OCR area"));
		row->removeButton = new QPushButton("×");
		row->removeButton->setToolTip(trText("Delete"));
		row->detailsToggle = new QToolButton();
		row->detailsToggle->setText(trText("Position & size"));
		row->detailsToggle->setCheckable(true);
		row->detailsToggle->setChecked(false);
		row->detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		row->detailsToggle->setArrowType(Qt::RightArrow);
		const double normalizedWidth = object.value("width").toDouble(METADATA_STANDARD_WIDTH);
		const double normalizedHeight = object.value("height").toDouble(METADATA_STANDARD_HEIGHT);
		row->x = createMetadataPositionSpin(
			metadataTopLeftToCenter(object.value("x").toDouble(0.0), normalizedWidth, METADATA_CANVAS_WIDTH),
			METADATA_CANVAS_WIDTH);
		row->y = createMetadataPositionSpin(
			metadataTopLeftToCenter(object.value("y").toDouble(0.0), normalizedHeight, METADATA_CANVAS_HEIGHT),
			METADATA_CANVAS_HEIGHT);
		row->width = createMetadataSizeSpin(normalizedWidth, METADATA_STANDARD_WIDTH);
		row->height = createMetadataSizeSpin(normalizedHeight, METADATA_STANDARD_HEIGHT);
		row->detailsPanel = new QWidget();
		auto *detailsLayout = new QGridLayout(row->detailsPanel);
		detailsLayout->setContentsMargins(0, 2, 0, 0);
		detailsLayout->setSpacing(6);

		layout->addWidget(row->name, 0, 0, 1, 2);
		layout->addWidget(row->value, 0, 2, 1, 2);
		layout->addWidget(row->pickButton, 0, 4);
		layout->addWidget(row->removeButton, 0, 5);
		layout->addWidget(row->detailsToggle, 1, 0, 1, 6);
		detailsLayout->addWidget(new QLabel("X"), 0, 0);
		detailsLayout->addWidget(row->x, 0, 1);
		detailsLayout->addWidget(new QLabel("Y"), 0, 2);
		detailsLayout->addWidget(row->y, 0, 3);
		detailsLayout->addWidget(new QLabel("W"), 1, 0);
		detailsLayout->addWidget(row->width, 1, 1);
		detailsLayout->addWidget(new QLabel("H"), 1, 2);
		detailsLayout->addWidget(row->height, 1, 3);
		row->detailsPanel->setVisible(false);
		layout->addWidget(row->detailsPanel, 2, 0, 1, 6);
		metadataFieldsLayout->insertWidget(std::max(0, metadataFieldsLayout->count() - 1), row->frame);
		metadataRows.push_back(row);

		auto save = [this]() {
			if (!metadataUiLoading)
				saveMetadataRowsToSettings();
		};
		connect(row->name, &QComboBox::editTextChanged, this, save);
		connect(row->value, &QLineEdit::textChanged, this, save);
		for (QDoubleSpinBox *box : {row->x, row->y, row->width, row->height})
			connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, save);
		connect(row->detailsToggle, &QToolButton::toggled, this, [row](bool checked) {
			if (row->detailsPanel)
				row->detailsPanel->setVisible(checked);
			if (row->detailsToggle)
				row->detailsToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
		});
		connect(row->pickButton, &QPushButton::clicked, this, [this, row]() { beginMetadataFieldSelection(row); });
		connect(row->removeButton, &QPushButton::clicked, this, [this, row]() {
			metadataRows.erase(std::remove(metadataRows.begin(), metadataRows.end(), row), metadataRows.end());
			if (activeMetadataPickRow == row)
				activeMetadataPickRow = nullptr;
			if (row->frame)
				row->frame->deleteLater();
			delete row;
			saveMetadataRowsToSettings();
		});
	}

	MetadataFieldRow *ensureMetadataFieldRow(const QString &name)
	{
		const QString cleanName = name.trimmed();
		if (cleanName.isEmpty())
			return nullptr;

		for (MetadataFieldRow *row : metadataRows) {
			if (row && row->name && row->name->currentText().trimmed().compare(cleanName, Qt::CaseInsensitive) == 0)
				return row;
		}

		QJsonObject object;
		object["name"] = cleanName;
		object["value"] = QString();
		addMetadataFieldRow(object);
		return metadataRows.empty() ? nullptr : metadataRows.back();
	}

	int applyClapperboardFields(const QByteArray &json)
	{
		const QJsonDocument doc = QJsonDocument::fromJson(json);
		if (!doc.isArray())
			return 0;

		int applied = 0;
		metadataUiLoading = true;
		for (const QJsonValue value : doc.array()) {
			const QJsonObject object = value.toObject();
			const QString name = object.value("name").toString().trimmed();
			const QString fieldValue = object.value("value").toString().trimmed();
			if (name.isEmpty() || fieldValue.isEmpty())
				continue;

			MetadataFieldRow *row = ensureMetadataFieldRow(name);
			if (!row || !row->value)
				continue;
			row->value->setText(fieldValue);
			const double width = object.value("width").toDouble(0.0);
			const double height = object.value("height").toDouble(0.0);
			if (width > 0.0 && height > 0.0) {
				if (row->width)
					row->width->setValue((width / METADATA_STANDARD_WIDTH) * 100.0);
				if (row->height)
					row->height->setValue((height / METADATA_STANDARD_HEIGHT) * 100.0);
				if (row->x)
					row->x->setValue(metadataTopLeftToCenter(object.value("x").toDouble(0.0), width,
										 METADATA_CANVAS_WIDTH));
				if (row->y)
					row->y->setValue(metadataTopLeftToCenter(object.value("y").toDouble(0.0), height,
										 METADATA_CANVAS_HEIGHT));
			}
			applied++;
		}
		metadataUiLoading = false;

		if (applied > 0)
			saveMetadataRowsToSettings();
		return applied;
	}

	QJsonArray metadataRowsJson() const
	{
		QJsonArray rows;
		for (const MetadataFieldRow *row : metadataRows) {
			if (!row || !row->name)
				continue;
			QJsonObject object;
			object["name"] = row->name->currentText().trimmed();
			object["value"] = row->value ? row->value->text() : QString();
			const double width = row->width ? metadataDisplayToNormalized(row->width->value(),
										      METADATA_STANDARD_WIDTH)
							: METADATA_STANDARD_WIDTH;
			const double height = row->height ? metadataDisplayToNormalized(row->height->value(),
											METADATA_STANDARD_HEIGHT)
							 : METADATA_STANDARD_HEIGHT;
			object["x"] = row->x ? metadataCenterToTopLeft(row->x->value(), METADATA_CANVAS_WIDTH, width)
					     : 0.0;
			object["y"] = row->y ? metadataCenterToTopLeft(row->y->value(), METADATA_CANVAS_HEIGHT, height)
					     : 0.0;
			object["width"] = width;
			object["height"] = height;
			rows.append(object);
		}
		return rows;
	}

	QJsonArray metadataPresetRowsJson() const
	{
		QJsonArray rows = metadataRowsJson();
		for (QJsonValueRef value : rows) {
			QJsonObject object = value.toObject();
			object["value"] = QString();
			value = object;
		}
		return rows;
	}

	void saveMetadataRowsToSettings()
	{
		const QJsonDocument doc(metadataRowsJson());
		const QByteArray json = doc.toJson(QJsonDocument::Compact);
		loadedMetadataJson = QString::fromUtf8(json);
		updateSettings([json](obs_data_t *settings) {
			obs_data_set_string(settings, "metadata_fields_json", json.constData());
		});
	}

	void loadMetadataRowsFromSettings(obs_data_t *settings)
	{
		if (!settings || metadataUiLoading)
			return;
		const char *raw = obs_data_get_string(settings, "metadata_fields_json");
		const QString json = raw ? QString::fromUtf8(raw) : QString();
		if (json == loadedMetadataJson)
			return;

		metadataUiLoading = true;
		clearMetadataRows();
		const QJsonDocument doc = QJsonDocument::fromJson(raw ? QByteArray(raw) : QByteArray());
		if (doc.isArray()) {
			for (const QJsonValue value : doc.array())
				addMetadataFieldRow(value.toObject());
		}
		loadedMetadataJson = json;
		metadataUiLoading = false;
	}

	void tagText(QWidget *widget, const char *key)
	{
		if (widget)
			widget->setProperty("trKey", key);
	}

	bool isMetadataSectionActive() const
	{
		return sectionPicker && sectionPicker->currentIndex() == 4;
	}

	void updateMetadataOverlayVisibility()
	{
		setBool("metadata_overlay_visible", isMetadataSectionActive());
	}

	QString trText(const QString &key) const
	{
		if (language == DockLanguage::English)
			return key;

		static const std::unordered_map<std::string, QString> fr = {
			{"General", "Général"},
			{"Detection Center", "Centre de détection"},
			{"Clip Name", "Clip Name"},
			{"Clip name", "Clip Name"},
			{"Presets", "Préréglages"},
			{"Metadata", "Métadonnées"},
			{"Clapperboard", "Clap"},
			{"No clap image yet", "Aucune image de clap"},
			{"Clapperboard OCR fills these fields from the current image.",
			 "L'OCR du clap remplit ces champs depuis l'image courante."},
			{"Auto-detect metadata", "Auto-détection metadata"},
			{"Auto-detect metadata requested...", "Auto-détection metadata demandée..."},
			{"Auto-detect filled metadata fields.", "Auto-détection a rempli des champs metadata."},
			{"Auto-detect found no usable metadata.", "Auto-détection n'a trouvé aucune metadata exploitable."},
			{"Clapperboard reading clap...", "Lecture du clap..."},
			{"Clapperboard filled fields.", "Le clap a rempli des champs."},
			{"Clapperboard found no usable data.", "Aucune donnée de clap exploitable trouvée."},
				{"Always armed on OBS launch", "Toujours armé au lancement d'OBS"},
				{"Support RecPilot", "Soutenir RecPilot"},
				{"I want to offer Bart a coffee", "Je souhaite offrir un café à Bart"},
				{"I want to offer Bart a coffee later", "Je souhaite offrir un café à Bart plus tard"},
				{"I already offered Bart a coffee", "J'ai déjà offert un café à Bart"},
				{"Are you sure you already offered Bart a coffee?",
				 "Êtes-vous sûr d'avoir bien offert un café à Bart ?"},
				{"OK, since you offered Bart a coffee, we will leave you alone. Thank you for your support!",
				 "OK, puisque vous avez offert un café à Bart, nous vous laissons tranquille. Merci de votre soutien !"},
				{"Add +1 to Clipname (ARRI camera)", "Ajouter +1 au Clipname (caméra ARRI)"},
				{"Save Clapperboard snapshots", "Enregistrer les snapshots Clap"},
			{"Color selection", "Sélection de couleur"},
			{"Red", "Rouge"},
			{"Green", "Vert"},
			{"Blue", "Bleu"},
			{"Yellow", "Jaune"},
			{"Orange", "Orange"},
			{"Magenta", "Magenta"},
			{"Cyan", "Cyan"},
			{"White", "Blanc"},
			{"Custom", "Personnalisé"},
			{"Pick from preview", "Choisir dans le preview"},
			{"Recording folder", "Dossier d'enregistrement"},
			{"Automatic folder", "Dossier automatique"},
			{"By date", "Par date"},
			{"By camera", "Par caméra"},
			{"By card", "Par carte"},
				{"Recording codec", "Codec d'enregistrement"},
				{"Recording resolution", "Résolution d'enregistrement"},
				{"Recording settings are locked while OBS is active.",
				 "Les réglages d'enregistrement sont verrouillés pendant qu'OBS est actif."},
				{"ZoeLog CSV per card", "CSV ZoeLog par carte"},
			{"Add metadata field", "Ajouter un champ metadata"},
			{"Manual value or OCR result", "Valeur manuelle ou résultat OCR"},
			{"Select OCR area", "Sélectionner la zone OCR"},
			{"Position & size", "Position et taille"},
			{"Click the metadata field center", "Cliquez au centre du champ metadata"},
			{"Metadata OCR area selected", "Zone OCR metadata sélectionnée"},
			{"Same as canvas", "Identique au canvas"},
			{"not usable at this resolution", "pas utilisable dans cette résolution"},
			{"OBS default folder", "Dossier OBS par défaut"},
			{"Choose...", "Choisir..."},
			{"Fine tuning", "Réglages fins"},
			{"Color dominance", "Dominance de couleur"},
			{"Minimum color coverage", "Couverture minimale de couleur"},
			{"Color detected frames before start", "Images couleur détectées avant démarrage"},
			{"Color lost frames before stop", "Images sans couleur avant arrêt"},
			{"Auto detect circle", "Détection automatique du cercle"},
			{"Automatic detection requested...", "Détection automatique demandée..."},
			{"Auto detection found the circle.", "Détection automatique réussie."},
			{"Center detected", "Centre détecté"},
			{"Center not found", "Centre non détecté"},
			{"Auto detect clip name", "Détection automatique du Clip Name"},
			{"Automatic clip name detection requested...",
			 "Détection automatique du Clip Name demandée..."},
			{"Clip name detected", "Clip Name détecté"},
			{"Clip name not found", "Clip Name non détecté"},
			{"Clip name detected.", "Clip Name détecté."},
			{"Clip name not found.", "Clip Name non détecté."},
			{"No matching circle found.", "Aucun cercle correspondant trouvé."},
			{"No matching circle found. Check the selected color.",
			 "Aucun cercle correspondant trouvé. Vérifiez la couleur sélectionnée."},
			{"No image received. Make sure the controlled source is visible.",
			 "Aucune image reçue. Vérifiez que la source contrôlée est visible."},
			{"Select in one click", "Sélection en un clic"},
			{"Detection center X", "Centre de détection X"},
			{"Detection center Y", "Centre de détection Y"},
			{"Detection radius", "Rayon de détection"},
			{"Replace O with 0", "Remplacer O par 0"},
			{"Replace spaces with underscores", "Remplacer les espaces par des underscores"},
			{"Remove spaces", "Supprimer les espaces"},
			{"Clip name X", "Clip name X"},
			{"Clip name Y", "Clip name Y"},
			{"Clip name width", "Largeur Clip name"},
			{"Clip name height", "Hauteur Clip name"},
			{"Load", "Charger"},
			{"Save", "Enregistrer"},
			{"Delete", "Supprimer"},
			{"Built-in camera layouts plus your saved layouts.",
			 "Layouts caméra intégrés, plus vos layouts enregistrés."},
			{"Layouts include Detection Center, Clip Name, and metadata fields.",
			 "Les layouts incluent le centre de détection, le Clip Name et les champs metadata."},
			{"Language", "Langue"},
			{"Click the detection point", "Cliquez sur le point à détecter"},
			{"Click the center of the Clip Name", "Cliquez au centre du Clip Name"},
			{"Click the color to detect", "Cliquez sur la couleur à détecter"},
			{"Rec", "Rec"},
			{"Stop", "Stop"},
			{"Armed", "Armé"},
			{"Disarmed", "Désarmé"},
			{"Overlay", "Overlay"},
			{"Hide", "Caché"},
		};

		static const std::unordered_map<std::string, QString> minion = {
			{"General", "Bello banana"},
			{"Detection Center", "Pika centro"},
			{"Clip Name", "Nom-nom clip"},
			{"Clip name", "Nom-nom clip"},
			{"Presets", "Papoy mémos"},
			{"Metadata", "Meta-banana"},
			{"Clapperboard", "CLAP-banana"},
			{"No clap image yet", "No image clap-banana"},
			{"Clapperboard OCR fills these fields from the current image.",
			 "CLAP-banana OCR remplit depuis image."},
			{"Auto-detect metadata", "Auto pika meta-banana"},
			{"Auto-detect metadata requested...", "Auto pika meta-banana demandé..."},
			{"Auto-detect filled metadata fields.", "Auto pika remplit meta-banana."},
			{"Auto-detect found no usable metadata.", "Auto pika no meta-banana trouvé."},
			{"Clapperboard reading clap...", "CLAP-banana lit clap..."},
			{"Clapperboard filled fields.", "CLAP-banana rempli champs."},
			{"Clapperboard found no usable data.", "CLAP-banana no data trouvé."},
				{"Always armed on OBS launch", "Toujours armé quand OBS bello"},
				{"Support RecPilot", "Soutenir RecPilot-bello"},
				{"I want to offer Bart a coffee", "Moi offrir café à Bart"},
				{"I want to offer Bart a coffee later", "Moi offrir café à Bart plus tard"},
				{"I already offered Bart a coffee", "Moi déjà offert café à Bart"},
				{"Are you sure you already offered Bart a coffee?", "Toi sûr café à Bart déjà donné ?"},
				{"OK, since you offered Bart a coffee, we will leave you alone. Thank you for your support!",
				 "OK, café Bart donné, nous stop papoy. Tank yu soutien-banana!"},
				{"Add +1 to Clipname (ARRI camera)", "Bap +1 nom-nom ARRI"},
				{"Save Clapperboard snapshots", "Garder snapshots CLAP-banana"},
			{"Color selection", "Couleur banana"},
			{"Red", "Roujo"},
			{"Green", "Verdo"},
			{"Blue", "Blu blu"},
			{"Yellow", "Banana"},
			{"Orange", "Oranjo"},
			{"Magenta", "Magenta-papoy"},
			{"Cyan", "Cyan-bello"},
			{"White", "Blanco"},
			{"Custom", "Moi-make"},
			{"Pick from preview", "Pika dans preview"},
			{"Recording folder", "Dodo dossier rec"},
			{"Automatic folder", "Dossier auto-bello"},
			{"By date", "Par date-banana"},
			{"By camera", "Par cam-cam"},
			{"By card", "Par carte-bello"},
				{"Recording codec", "Codec rec-bap"},
				{"Recording resolution", "Résolushun"},
				{"Recording settings are locked while OBS is active.", "Réglages rec dodo pendant OBS actif."},
				{"ZoeLog CSV per card", "CSV ZoeLog par carte-banana"},
			{"Add metadata field", "Ajouter meta-banana"},
			{"Manual value or OCR result", "Valeur main ou OCR-bello"},
			{"Select OCR area", "Pika zone OCR"},
			{"Position & size", "Posishun e tamaño"},
			{"Click the metadata field center", "Pika centro meta-banana"},
			{"Metadata OCR area selected", "Zone OCR meta-banana choisie"},
			{"Same as canvas", "Même canvas, papoy"},
			{"not usable at this resolution", "no no à cette résolushun"},
			{"OBS default folder", "Dossier OBS dodo"},
			{"Choose...", "Choosito..."},
			{"Fine tuning", "Tiki réglage"},
			{"Color dominance", "Couleur boss"},
			{"Minimum color coverage", "Mini couleur partout"},
			{"Color detected frames before start", "Frames couleur avant go-go"},
			{"Color lost frames before stop", "Frames no couleur avant stop"},
			{"Auto detect circle", "Auto pika rond"},
			{"Automatic detection requested...", "Auto pika demandé..."},
			{"Auto detection found the circle.", "Auto pika rond bello!"},
			{"Center detected", "Centro trouvé, banana!"},
			{"Center not found", "Centro no trouvé"},
			{"Auto detect clip name", "Auto pika nom-nom clip"},
			{"Automatic clip name detection requested...", "Auto pika nom-nom demandé..."},
			{"Clip name detected", "Nom-nom clip trouvé!"},
			{"Clip name not found", "Nom-nom clip no trouvé"},
			{"Clip name detected.", "Nom-nom clip trouvé!"},
			{"Clip name not found.", "Nom-nom clip no trouvé."},
			{"No matching circle found.", "No rond pareil trouvé."},
			{"No matching circle found. Check the selected color.", "No rond pareil. Check couleur banana."},
			{"No image received. Make sure the controlled source is visible.",
			 "No image reçue. Source visible, por favor."},
			{"Select in one click", "Pika one click"},
			{"Detection center X", "Centro pika X"},
			{"Detection center Y", "Centro pika Y"},
			{"Detection radius", "Rayon pika"},
			{"Replace O with 0", "O devient 0, hehe"},
			{"Replace spaces with underscores", "Spaces deviennent underscores"},
			{"Remove spaces", "Bye-bye spaces"},
			{"Clip name X", "Nom-nom X"},
			{"Clip name Y", "Nom-nom Y"},
			{"Clip name width", "Largeur nom-nom"},
			{"Clip name height", "Hauteur nom-nom"},
			{"Load", "Go chercher"},
			{"Save", "Garder banana"},
			{"Delete", "Pouf"},
			{"Built-in camera layouts plus your saved layouts.", "Layouts cam-cam + tes mémos banana."},
			{"Layouts include Detection Center, Clip Name, and metadata fields.",
			 "Layouts centro, nom-nom clip e meta-banana."},
			{"Language", "Langajo"},
			{"Click the detection point", "Pika point detection"},
			{"Click the center of the Clip Name", "Pika centro nom-nom"},
			{"Click the color to detect", "Pika couleur"},
			{"Rec", "Rec-bello"},
			{"Stop", "Stopa"},
			{"Armed", "Armado"},
			{"Disarmed", "Dodo"},
			{"Overlay", "Ové-lay"},
			{"Hide", "Cache-cache"},
		};

		const auto &translations = language == DockLanguage::French ? fr : minion;
		const auto it = translations.find(key.toStdString());
		return it == translations.end() ? key : it->second;
	}

	void applyLanguage()
	{
		for (QWidget *widget : findChildren<QWidget *>()) {
			const QString key = widget->property("trKey").toString();
			if (key.isEmpty())
				continue;

			if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
				const QString prefix = button->icon().isNull() ? QString() : QString("  ");
				button->setText(prefix + trText(key));
			} else if (auto *label = qobject_cast<QLabel *>(widget)) {
				label->setText(trText(key));
			}
		}

		if (sectionPicker) {
			QSignalBlocker blocker(sectionPicker);
			const int current = sectionPicker->currentIndex();
			const QStringList keys = {"General", "Detection Center", "Clip Name", "Presets", "Metadata",
						  "Clapperboard"};
			for (int i = 0; i < keys.size() && i < sectionPicker->count(); ++i)
				sectionPicker->setItemText(i, trText(keys[i]));
			sectionPicker->setCurrentIndex(current);
		}

		if (colorPreset) {
			QSignalBlocker blocker(colorPreset);
			const int current = colorPreset->currentIndex();
			const QStringList keys = {"Red", "Green", "Blue", "Yellow", "Orange", "Magenta", "Cyan",
						  "White", "Custom"};
			for (int i = 0; i < keys.size() && i < colorPreset->count(); ++i)
				colorPreset->setItemText(i, trText(keys[i]));
			colorPreset->setCurrentIndex(current);
		}

			if (recordingFolderPath)
				recordingFolderPath->setPlaceholderText(trText("OBS default folder"));
			if (coffeeButton)
				coffeeButton->setToolTip(trText("Support RecPilot"));
			if (clapperboardButton)
				clapperboardButton->setToolTip(trText("Clapperboard"));
			if (languageButton) {
			if (language == DockLanguage::Minion) {
				languageButton->setIcon(yellowBuddyIcon());
				languageButton->setIconSize(QSize(22, 22));
				languageButton->setText("  Minion");
			} else {
				languageButton->setIcon(QIcon());
				languageButton->setText(language == DockLanguage::English ? "🇬🇧 English" : "🇫🇷 Français");
			}
		}

		refreshRecordingResolutionLabels();
		refreshRecordingCodecAvailability();
		updateAutoFolderUi();
		updateFineTuneToggle();
		updateTopToggleButtons();
		updatePreviewResultToastText();
	}

	void updateAutoFolderUi()
	{
		const bool enabled = autoFolderEnabled && autoFolderEnabled->isEnabled() && autoFolderEnabled->isChecked();
		if (autoFolderByDate)
			autoFolderByDate->setEnabled(enabled);
		if (autoFolderByCamera)
			autoFolderByCamera->setEnabled(enabled);
		if (autoFolderByCard)
			autoFolderByCard->setEnabled(enabled);
	}

		void updateFineTuneToggle()
		{
			if (!fineTuneToggle)
				return;
			const bool expanded = fineTuneToggle->isChecked();
			fineTuneToggle->setText(QString("%1 %2").arg(expanded ? "▾" : "▸", trText("Fine tuning")));
		}

		bool coffeeThanksConfirmed() const
		{
			if (!filter)
				return false;
			obs_data_t *settings = obs_source_get_settings(filter);
			if (!settings)
				return false;
			const bool confirmed = obs_data_get_bool(settings, "coffee_thanks_confirmed");
			obs_data_release(settings);
			return confirmed;
		}

		void markCoffeeThanksConfirmed()
		{
			setBool("coffee_thanks_confirmed", true);
			if (coffeeReminderTimer)
				coffeeReminderTimer->stop();
		}

		static QString todayKey()
		{
			return QDate::currentDate().toString(Qt::ISODate);
		}

		int coffeeReminderCountToday(obs_data_t *settings) const
		{
			const QString today = todayKey();
			const QString storedDay = QString::fromUtf8(obs_data_get_string(settings, "coffee_reminder_day"));
			if (storedDay == today)
				return static_cast<int>(obs_data_get_int(settings, "coffee_reminder_count"));

			obs_data_set_string(settings, "coffee_reminder_day", today.toUtf8().constData());
			obs_data_set_int(settings, "coffee_reminder_count", 0);
			return 0;
		}

		void scheduleCoffeeReminder(bool resetDelay)
		{
			if (!coffeeReminderTimer || !filter || coffeeThanksConfirmed())
				return;
			if (coffeeReminderTimer->isActive() && !resetDelay)
				return;

			obs_data_t *settings = obs_source_get_settings(filter);
			if (!settings)
				return;

			const int remindersToday = coffeeReminderCountToday(settings);
			obs_source_update(filter, settings);
			obs_data_release(settings);
			if (remindersToday >= 2) {
				coffeeReminderTimer->stop();
				return;
			}

			const int minutes = remindersToday == 0 ? QRandomGenerator::global()->bounded(5, 16)
								: QRandomGenerator::global()->bounded(90, 301);
			coffeeReminderTimer->start(minutes * 60 * 1000);
		}

		void noteCoffeeReminderShown()
		{
			if (!filter)
				return;

			updateSettings([this](obs_data_t *settings) {
				const int remindersToday = coffeeReminderCountToday(settings);
				obs_data_set_string(settings, "coffee_reminder_day", todayKey().toUtf8().constData());
				obs_data_set_int(settings, "coffee_reminder_count", std::min(2, remindersToday + 1));
			});
		}

		void showCoffeeDialog(bool automatic = false)
		{
			if (automatic)
				noteCoffeeReminderShown();

			if (coffeeDialogVisible) {
				scheduleCoffeeReminder(true);
				return;
			}

			if (coffeeThanksConfirmed()) {
				if (!automatic)
					QMessageBox::information(
						this, trText("Support RecPilot"),
						trText("OK, since you offered Bart a coffee, we will leave you alone. Thank you for your support!"));
				return;
			}

			coffeeDialogVisible = true;
			QDialog dialog(this);
			dialog.setWindowTitle(trText("Support RecPilot"));
			auto *layout = new QVBoxLayout(&dialog);
			layout->setContentsMargins(18, 18, 18, 18);
			layout->setSpacing(10);

			auto *header = new QHBoxLayout();
			auto *coffeeLogo = new QLabel();
			coffeeLogo->setPixmap(kofiIcon().pixmap(QSize(34, 28)));
			coffeeLogo->setFixedSize(38, 32);
			coffeeLogo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
			header->addWidget(coffeeLogo, 0, Qt::AlignLeft);
			header->addStretch();
			auto *brand = new QLabel(APP_NAME);
			brand->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			brand->setStyleSheet("font-size: 18px; font-weight: 800;");
			header->addWidget(brand, 0, Qt::AlignRight);
			layout->addLayout(header);

			auto *offerNow = new QPushButton(trText("I want to offer Bart a coffee"));
			auto *offerLater = new QPushButton(trText("I want to offer Bart a coffee later"));
			auto *alreadyOffered = new QPushButton(trText("I already offered Bart a coffee"));
			layout->addWidget(offerNow);
			layout->addWidget(offerLater);
			layout->addWidget(alreadyOffered);

			int choice = 0;
			connect(offerNow, &QPushButton::clicked, &dialog, [&]() {
				choice = 1;
				dialog.accept();
			});
			connect(offerLater, &QPushButton::clicked, &dialog, [&]() {
				choice = 2;
				dialog.accept();
			});
			connect(alreadyOffered, &QPushButton::clicked, &dialog, [&]() {
				choice = 3;
				dialog.accept();
			});

			dialog.exec();
			coffeeDialogVisible = false;

			if (choice == 1) {
				QDesktopServices::openUrl(QUrl("https://ko-fi.com/bart57662"));
				scheduleCoffeeReminder(true);
			} else if (choice == 2 || choice == 0) {
				scheduleCoffeeReminder(true);
			} else if (choice == 3) {
				const QMessageBox::StandardButton answer =
					QMessageBox::question(this, trText("Support RecPilot"),
							      trText("Are you sure you already offered Bart a coffee?"),
							      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
				if (answer == QMessageBox::Yes) {
					markCoffeeThanksConfirmed();
					QMessageBox::information(
						this, trText("Support RecPilot"),
						trText("OK, since you offered Bart a coffee, we will leave you alone. Thank you for your support!"));
				} else {
					scheduleCoffeeReminder(true);
				}
			}
		}

		static QPushButton *createTopToggleButton(const QString &text, const QString &tooltip)
		{
		auto *button = new QPushButton(text);
		button->setObjectName("TopToggle");
		button->setCheckable(true);
		button->setToolTip(tooltip);
		button->setProperty("active", false);
		return button;
	}

	static QLabel *createTopButtonLabel(const QString &text)
	{
		auto *label = new QLabel(text);
		label->setObjectName("TopButtonLabel");
		label->setAlignment(Qt::AlignCenter);
		label->setMinimumWidth(58);
		return label;
	}

	static QVBoxLayout *createTopButtonColumn(QPushButton *button, QLabel *label)
	{
		auto *column = new QVBoxLayout();
		column->setSpacing(4);
		column->setAlignment(Qt::AlignCenter);
		column->addWidget(button, 0, Qt::AlignCenter);
		column->addWidget(label, 0, Qt::AlignCenter);
		return column;
	}

	static QIcon cursorIcon()
	{
		QPixmap pixmap(48, 48);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing, true);

		QPainterPath path;
		path.moveTo(9.0, 6.0);
		path.lineTo(9.0, 38.0);
		path.cubicTo(9.0, 42.0, 13.8, 43.9, 16.5, 40.9);
		path.lineTo(22.0, 34.7);
		path.lineTo(26.8, 45.1);
		path.cubicTo(27.8, 47.1, 30.1, 48.0, 32.1, 47.0);
		path.lineTo(39.1, 43.7);
		path.cubicTo(41.1, 42.8, 42.0, 40.4, 41.0, 38.4);
		path.lineTo(36.0, 27.7);
		path.lineTo(44.0, 26.6);
		path.cubicTo(47.8, 26.1, 49.3, 21.4, 46.4, 18.8);
		path.lineTo(16.5, 3.1);
		path.cubicTo(13.6, 0.5, 9.0, 2.6, 9.0, 6.0);

		painter.setPen(QPen(QColor(35, 37, 42), 5.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter.setBrush(QColor(238, 239, 242));
		painter.drawPath(path);

		return QIcon(pixmap);
	}

		static QIcon yellowBuddyIcon()
		{
		QPixmap pixmap(48, 48);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing, true);

		QPainterPath body;
		body.addRoundedRect(QRectF(9.0, 5.0, 30.0, 38.0), 14.0, 14.0);
		painter.setPen(QPen(QColor(83, 75, 40), 2.5));
		painter.setBrush(QColor(255, 217, 36));
		painter.drawPath(body);

		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(45, 61, 88));
		painter.drawRect(QRectF(9.0, 22.0, 30.0, 7.0));
		painter.setBrush(QColor(225, 232, 240));
		painter.drawEllipse(QRectF(15.0, 15.0, 18.0, 18.0));
		painter.setBrush(QColor(77, 86, 96));
		painter.drawEllipse(QRectF(18.0, 18.0, 12.0, 12.0));
		painter.setBrush(QColor(24, 26, 30));
		painter.drawEllipse(QRectF(21.0, 21.0, 6.0, 6.0));

		painter.setPen(QPen(QColor(95, 63, 28), 2.0, Qt::SolidLine, Qt::RoundCap));
		painter.drawLine(QPointF(18.0, 36.0), QPointF(30.0, 36.0));

			return QIcon(pixmap);
		}

		static QIcon kofiIcon()
		{
			char *path = obs_module_file("kofi_logo.png");
			if (path) {
				QPixmap pixmap(QString::fromUtf8(path));
				bfree(path);
				if (!pixmap.isNull())
					return QIcon(pixmap);
			}

			QPixmap pixmap(48, 48);
			pixmap.fill(Qt::transparent);
			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setPen(QPen(QColor(35, 37, 42), 4.0));
			painter.setBrush(Qt::white);
			painter.drawRoundedRect(QRectF(6.0, 10.0, 34.0, 27.0), 10.0, 10.0);
			painter.drawArc(QRectF(31.0, 14.0, 13.0, 15.0), -80 * 16, 230 * 16);
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(255, 95, 24));
			QPainterPath heart;
			heart.moveTo(24.0, 31.0);
			heart.cubicTo(15.0, 24.0, 13.0, 18.0, 18.0, 16.0);
			heart.cubicTo(21.0, 14.8, 23.2, 17.0, 24.0, 18.5);
			heart.cubicTo(24.8, 17.0, 27.0, 14.8, 30.0, 16.0);
			heart.cubicTo(35.0, 18.0, 33.0, 24.0, 24.0, 31.0);
			painter.drawPath(heart);
			return QIcon(pixmap);
		}

		static QIcon clapIcon()
		{
			QPixmap pixmap(48, 48);
			pixmap.fill(Qt::transparent);

			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setPen(QPen(QColor(230, 234, 242), 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			painter.setBrush(QColor(35, 40, 50));
			painter.drawRoundedRect(QRectF(8.0, 18.0, 32.0, 22.0), 3.0, 3.0);

			painter.setBrush(QColor(230, 234, 242));
			painter.drawRect(QRectF(8.0, 18.0, 32.0, 7.0));
			painter.setPen(QPen(QColor(35, 40, 50), 3.0));
			for (int i = -2; i < 5; ++i)
				painter.drawLine(QPointF(8.0 + i * 10.0, 25.0), QPointF(15.0 + i * 10.0, 18.0));

			painter.setPen(QPen(QColor(230, 234, 242), 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			painter.setBrush(QColor(35, 40, 50));
			QPolygonF top;
			top << QPointF(8.0, 16.0) << QPointF(38.0, 7.0) << QPointF(40.0, 14.0) << QPointF(10.0, 23.0);
			painter.drawPolygon(top);
			painter.setPen(QPen(QColor(230, 234, 242), 2.5));
			for (int i = -1; i < 5; ++i)
				painter.drawLine(QPointF(9.0 + i * 9.0, 20.0), QPointF(15.0 + i * 9.0, 11.0));

			painter.setPen(QPen(QColor(82, 92, 112), 1.5));
			painter.drawLine(QPointF(13.0, 31.0), QPointF(35.0, 31.0));
			painter.drawLine(QPointF(13.0, 36.0), QPointF(30.0, 36.0));
			return QIcon(pixmap);
		}

	static QIcon fitIcon(bool outward)
	{
		QPixmap pixmap(48, 48);
		pixmap.fill(Qt::transparent);
		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(QPen(QColor(230, 234, 242), 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

		const QPointF center(24.0, 24.0);
		const std::array<QPointF, 4> corners = {QPointF(10.0, 10.0), QPointF(38.0, 10.0),
							QPointF(10.0, 38.0), QPointF(38.0, 38.0)};
		for (const QPointF &corner : corners) {
			const QPointF start = outward ? center + (corner - center) * 0.35 : corner;
			const QPointF end = outward ? corner : center + (corner - center) * 0.35;
			painter.drawLine(start, end);
			const double sx = corner.x() < center.x() ? -1.0 : 1.0;
			const double sy = corner.y() < center.y() ? -1.0 : 1.0;
			const QPointF tip = outward ? end : end;
			painter.drawLine(tip, tip - QPointF(8.0 * sx, 0.0));
			painter.drawLine(tip, tip - QPointF(0.0, 8.0 * sy));
		}
		return QIcon(pixmap);
	}

	static QIcon rotateIcon()
	{
		QPixmap pixmap(48, 48);
		pixmap.fill(Qt::transparent);
		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(QPen(QColor(230, 234, 242), 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter.drawArc(QRectF(10.0, 10.0, 28.0, 28.0), 35 * 16, 285 * 16);
		QPolygonF arrow;
		arrow << QPointF(36.0, 13.0) << QPointF(36.0, 25.0) << QPointF(44.0, 18.0);
		painter.setBrush(QColor(230, 234, 242));
		painter.setPen(Qt::NoPen);
		painter.drawPolygon(arrow);
		return QIcon(pixmap);
	}

	static int colorCode(int r, int g, int b)
	{
		return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
	}

	static QColor colorFromCode(int code)
	{
		return QColor((code >> 16) & 0xFF, (code >> 8) & 0xFF, code & 0xFF);
	}

	void addColorPreset(const QString &name, int r, int g, int b)
	{
		if (!colorPreset)
			return;
		colorPreset->addItem(name, r < 0 ? -1 : colorCode(r, g, b));
	}

	void updateColorUi(int r, int g, int b)
	{
		const QColor color(r, g, b);
		if (colorSwatch) {
			colorSwatch->setStyleSheet(QString("background: %1; border: 1px solid #6a7280; border-radius: 5px;")
							   .arg(color.name()));
			colorSwatch->setToolTip(QString("Current color: %1").arg(color.name(QColor::HexRgb).toUpper()));
		}
		if (!colorPreset)
			return;

		const int code = colorCode(r, g, b);
		int match = -1;
		for (int i = 0; i < colorPreset->count(); ++i) {
			if (colorPreset->itemData(i).toInt() == code) {
				match = i;
				break;
			}
		}
		if (match < 0)
			match = colorPreset->findText("Custom");

		QSignalBlocker blocker(colorPreset);
		colorPreset->setCurrentIndex(match);
	}

	void setDetectionColor(const QColor &color)
	{
		if (!color.isValid())
			return;

		updateColorUi(color.red(), color.green(), color.blue());
		updateSettings([&color](obs_data_t *settings) {
			obs_data_set_int(settings, "color_r", color.red());
			obs_data_set_int(settings, "color_g", color.green());
			obs_data_set_int(settings, "color_b", color.blue());
			obs_data_set_bool(settings, "color_pick_pending", false);
		});
	}

	void applyColorPreset(int index)
	{
		if (!colorPreset || index < 0)
			return;
		const int code = colorPreset->itemData(index).toInt();
		if (code < 0)
			return;
		setDetectionColor(colorFromCode(code));
	}

	void chooseCustomColor()
	{
		obs_data_t *settings = filter ? obs_source_get_settings(filter) : nullptr;
		const QColor current(settings ? static_cast<int>(obs_data_get_int(settings, "color_r")) : 255,
				     settings ? static_cast<int>(obs_data_get_int(settings, "color_g")) : 0,
				     settings ? static_cast<int>(obs_data_get_int(settings, "color_b")) : 0);
		if (settings)
			obs_data_release(settings);

		const QColor color = QColorDialog::getColor(current, this, "Choose detection color");
		setDetectionColor(color);
	}

	static QString currentRecordingFolder()
	{
		config_t *config = obs_frontend_get_profile_config();
		if (!config)
			return {};

		const char *rootPath = config_get_string(config, "RecPilot", "RootFolder");
		if (rootPath && *rootPath)
			return QString::fromUtf8(rootPath);

		const char *mode = config_get_string(config, "Output", "Mode");
		const bool advanced = mode && strcmp(mode, "Advanced") == 0;
		const char *path = advanced ? config_get_string(config, "AdvOut", "RecFilePath")
					    : config_get_string(config, "SimpleOutput", "FilePath");
		return path ? QString::fromUtf8(path) : QString();
	}

	void loadRecordingFolder()
	{
		if (!recordingFolderPath)
			return;

		QSignalBlocker blocker(recordingFolderPath);
		recordingFolderPath->setText(QDir::toNativeSeparators(currentRecordingFolder()));
	}

	void chooseRecordingFolder()
	{
		if (recordingSettingsLocked()) {
			setTemporaryStatus(trText("Recording settings are locked while OBS is active."), 4);
			loadRecordingFolder();
			return;
		}

		const QString startDir = currentRecordingFolder().isEmpty() ? QDir::homePath()
									    : currentRecordingFolder();
		const QString folder =
			QFileDialog::getExistingDirectory(this, "Choose recording folder", startDir,
							  QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
		if (folder.isEmpty())
			return;

		config_t *config = obs_frontend_get_profile_config();
		if (!config)
			return;

		const QByteArray path = QDir::toNativeSeparators(folder).toUtf8();
		config_set_string(config, "RecPilot", "RootFolder", path.constData());
		config_set_string(config, "SimpleOutput", "FilePath", path.constData());
		config_set_string(config, "AdvOut", "RecFilePath", path.constData());
		config_set_string(config, "AdvOut", "FFFilePath", path.constData());
		config_save(config);
		loadRecordingFolder();
	}

	static QString currentRecordingCodec()
	{
		config_t *config = obs_frontend_get_profile_config();
		if (!config)
			return {};

		const char *mode = config_get_string(config, "Output", "Mode");
		const bool advanced = mode && strcmp(mode, "Advanced") == 0;
		if (advanced) {
			const char *type = config_get_string(config, "AdvOut", "RecType");
			const bool ffmpeg = type && strcmp(type, "FFmpeg") == 0;
			if (ffmpeg) {
				const QString encoder =
					QString::fromUtf8(config_get_string(config, "AdvOut", "FFVEncoder"));
				const QString custom =
					QString::fromUtf8(config_get_string(config, "AdvOut", "FFVCustom"));
				if (encoder.contains("prores", Qt::CaseInsensitive))
					return custom.contains("profile=1") ? "prores_lt" : "prores_hq";
				if (encoder.contains("hevc", Qt::CaseInsensitive) ||
				    encoder.contains("h265", Qt::CaseInsensitive))
					return "h265";
			}
		}

		const QString simpleEncoder =
			QString::fromUtf8(config_get_string(config, "SimpleOutput", "RecEncoder"));
		if (simpleEncoder.contains("hevc", Qt::CaseInsensitive) ||
		    simpleEncoder.contains("h265", Qt::CaseInsensitive))
			return "h265";
		return "prores_hq";
	}

	void loadRecordingCodec()
	{
		if (!recordingCodec)
			return;

		const int index = recordingCodec->findData(currentRecordingCodec());
		QSignalBlocker blocker(recordingCodec);
		recordingCodec->setCurrentIndex(index >= 0 ? index : 0);
		refreshRecordingCodecAvailability();
	}

	static QString resolutionFamily(unsigned long long width, unsigned long long height)
	{
		if (width == 720 && (height == 576 || height == 480))
			return "SD";
		if (width == 1280 && height == 720)
			return "720p";
		if (width == 1920 && height == 1080)
			return "HD";
		if (width == 2048 && height == 1080)
			return "2K";
		if (width == 3840 && height == 2160)
			return "UHD";
		if (width == 4096 && height == 2160)
			return "4K DCI";
		return QString("%1x%2").arg(width).arg(height);
	}

	static QString currentCanvasResolutionLabel()
	{
		config_t *config = obs_frontend_get_profile_config();
		if (!config)
			return "HD";

		const auto width = config_get_uint(config, "Video", "BaseCX");
		const auto height = config_get_uint(config, "Video", "BaseCY");
		if (!width || !height)
			return "HD";

		return resolutionFamily(width, height);
	}

	void refreshRecordingResolutionLabels()
	{
		if (!recordingResolution)
			return;

		QSignalBlocker blocker(recordingResolution);
		const int sameCanvasIndex = recordingResolution->findData("same_canvas");
		if (sameCanvasIndex >= 0) {
			recordingResolution->setItemText(
				sameCanvasIndex,
				QString("%1 (%2)").arg(trText("Same as canvas"), currentCanvasResolutionLabel()));
		}
	}

	static bool parseResolution(const QString &resolution, unsigned long long &width, unsigned long long &height)
	{
		const QStringList parts = resolution.split('x');
		if (parts.size() != 2)
			return false;

		bool widthOk = false;
		bool heightOk = false;
		width = parts[0].toULongLong(&widthOk);
		height = parts[1].toULongLong(&heightOk);
		return widthOk && heightOk && width && height;
	}

	static QString resolveRecordingResolution(const QString &resolution)
	{
		if (resolution != "same_canvas")
			return resolution;

		config_t *config = obs_frontend_get_profile_config();
		if (!config)
			return {};

		const auto baseWidth = config_get_uint(config, "Video", "BaseCX");
		const auto baseHeight = config_get_uint(config, "Video", "BaseCY");
		if (!baseWidth || !baseHeight)
			return {};

		return QString("%1x%2").arg(baseWidth).arg(baseHeight);
	}

	static bool proResSupportsResolution(const QString &resolution)
	{
		unsigned long long width = 0;
		unsigned long long height = 0;
		if (!parseResolution(resolveRecordingResolution(resolution), width, height))
			return true;

		return (width == 720 && (height == 576 || height == 480)) || (width == 1280 && height == 720) ||
		       (width == 1920 && height == 1080) || (width == 2048 && height == 1080) ||
		       (width == 3840 && height == 2160) || (width == 4096 && height == 2160);
	}

	static QString codecBaseLabel(const QString &codec)
	{
		if (codec == "prores_hq")
			return "ProRes HQ";
		if (codec == "prores_lt")
			return "ProRes LT";
		return "H265";
	}

	void refreshRecordingCodecAvailability()
	{
		if (!recordingCodec || !recordingResolution)
			return;

		const QString resolution = recordingResolution->currentData().toString();
		const bool proResAllowed = proResSupportsResolution(resolution);
		auto *model = qobject_cast<QStandardItemModel *>(recordingCodec->model());

		QSignalBlocker blocker(recordingCodec);
		for (int i = 0; i < recordingCodec->count(); ++i) {
			const QString codec = recordingCodec->itemData(i).toString();
			const bool enabled = !codec.startsWith("prores") || proResAllowed;
			QString label = codecBaseLabel(codec);
			if (!enabled)
				label += QString(" (%1)").arg(trText("not usable at this resolution"));

			recordingCodec->setItemText(i, label);
			if (model) {
				if (auto *item = model->item(i))
					item->setFlags(enabled ? Qt::ItemIsSelectable | Qt::ItemIsEnabled
							       : Qt::NoItemFlags);
			} else {
				recordingCodec->setItemData(i, enabled ? QVariant() : QVariant(0), Qt::UserRole - 1);
			}
		}

		const QString selectedCodec = recordingCodec->currentData().toString();
		if (selectedCodec.startsWith("prores") && !proResAllowed) {
			const int h265Index = recordingCodec->findData("h265");
			if (h265Index >= 0) {
				recordingCodec->setCurrentIndex(h265Index);
				pendingRecordingCodecApply = true;
				if (recordingOutputUpdatesReady) {
					blocker.unblock();
					pendingRecordingCodecApply = false;
					applyRecordingCodec(h265Index);
				}
			}
		}
	}

	static QString currentRecordingResolution()
	{
		config_t *config = obs_frontend_get_profile_config();
		if (!config)
			return "same_canvas";

		const auto width = config_get_uint(config, "Video", "OutputCX");
		const auto height = config_get_uint(config, "Video", "OutputCY");
		const auto baseWidth = config_get_uint(config, "Video", "BaseCX");
		const auto baseHeight = config_get_uint(config, "Video", "BaseCY");
		if (!width || !height)
			return "same_canvas";
		if (baseWidth && baseHeight && width == baseWidth && height == baseHeight)
			return "same_canvas";

		return QString("%1x%2").arg(width).arg(height);
	}

	void loadRecordingResolution()
	{
		if (!recordingResolution)
			return;

		refreshRecordingResolutionLabels();
		const int index = recordingResolution->findData(currentRecordingResolution());
		QSignalBlocker blocker(recordingResolution);
		recordingResolution->setCurrentIndex(index >= 0 ? index : 0);
		refreshRecordingCodecAvailability();
	}

	static void resetVideoIfIdle()
	{
		if (recordingSettingsLocked())
			return;

		obs_frontend_reset_video();
	}

	static bool recordingSettingsLocked()
	{
		return obs_frontend_recording_active() || obs_frontend_streaming_active() ||
		       obs_frontend_replay_buffer_active();
	}

	static void updateRecordingOutputIfIdle(config_t *config)
	{
		if (!config || recordingSettingsLocked())
			return;

		obs_output_t *output = obs_frontend_get_recording_output();
		if (!output)
			return;

		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "format_name", config_get_string(config, "AdvOut", "FFFormat"));
		obs_data_set_string(settings, "format_mime_type",
				    config_get_string(config, "AdvOut", "FFFormatMimeType"));
		obs_data_set_string(settings, "video_encoder", config_get_string(config, "AdvOut", "FFVEncoder"));
		obs_data_set_int(settings, "video_encoder_id", config_get_int(config, "AdvOut", "FFVEncoderId"));
		obs_data_set_string(settings, "video_settings", config_get_string(config, "AdvOut", "FFVCustom"));
		obs_data_set_int(settings, "video_bitrate", config_get_int(config, "AdvOut", "FFVBitrate"));
		obs_data_set_string(settings, "audio_encoder", config_get_string(config, "AdvOut", "FFAEncoder"));
		obs_data_set_int(settings, "audio_encoder_id", config_get_int(config, "AdvOut", "FFAEncoderId"));
		obs_data_set_string(settings, "audio_settings", config_get_string(config, "AdvOut", "FFACustom"));
		obs_data_set_int(settings, "audio_bitrate", config_get_int(config, "AdvOut", "FFABitrate"));
		obs_output_set_mixers(output, config_get_int(config, "AdvOut", "FFAudioMixes"));
		obs_output_update(output, settings);
		obs_data_release(settings);
	}

	void applyRecordingCodec(int index)
	{
		if (!recordingCodec || index < 0)
			return;
		if (!recordingOutputUpdatesReady) {
			pendingRecordingCodecApply = true;
			return;
		}
		if (recordingSettingsLocked()) {
			setTemporaryStatus(trText("Recording settings are locked while OBS is active."), 4);
			loadRecordingCodec();
			return;
		}

		config_t *config = obs_frontend_get_profile_config();
		if (!config)
			return;

		const QString codec = recordingCodec->itemData(index).toString();
		config_set_string(config, "Output", "Mode", "Advanced");
		config_set_string(config, "AdvOut", "RecType", "FFmpeg");
		config_set_bool(config, "AdvOut", "FFOutputToFile", true);
		config_set_uint(config, "AdvOut", "FFAudioMixes", 1);
		config_set_uint(config, "AdvOut", "FFABitrate", 320);
		config_set_string(config, "AdvOut", "FFAEncoder", "aac");
		config_set_int(config, "AdvOut", "FFAEncoderId", 0);
		config_set_string(config, "AdvOut", "FFACustom", "");

		if (codec == "h265") {
			config_set_string(config, "AdvOut", "FFFormat", "mp4");
			config_set_string(config, "AdvOut", "FFFormatMimeType", "video/mp4");
			config_set_string(config, "AdvOut", "FFExtension", "mp4");
			config_set_string(config, "AdvOut", "FFVEncoder", "hevc_videotoolbox");
			config_set_int(config, "AdvOut", "FFVEncoderId", 0);
			config_set_string(config, "AdvOut", "FFVCustom", "b=12000k");
			config_set_int(config, "AdvOut", "FFVBitrate", 12000);
			config_set_string(config, "SimpleOutput", "RecFormat2", "mp4");
			config_set_string(config, "SimpleOutput", "RecEncoder", "apple_hevc");
		} else {
			const bool lt = codec == "prores_lt";
			config_set_string(config, "AdvOut", "FFFormat", "mov");
			config_set_string(config, "AdvOut", "FFFormatMimeType", "video/quicktime");
			config_set_string(config, "AdvOut", "FFExtension", "mov");
			config_set_string(config, "AdvOut", "FFVEncoder", "prores_ks");
			config_set_int(config, "AdvOut", "FFVEncoderId", 0);
			config_set_string(config, "AdvOut", "FFVCustom", lt ? "profile=1" : "profile=3");
			config_set_int(config, "AdvOut", "FFVBitrate", 0);
			config_set_string(config, "SimpleOutput", "RecFormat2", "mov");
		}

		config_save(config);
		updateRecordingOutputIfIdle(config);
	}

	void applyRecordingResolution(int index)
	{
		if (!recordingResolution || index < 0)
			return;
		if (recordingSettingsLocked()) {
			setTemporaryStatus(trText("Recording settings are locked while OBS is active."), 4);
			loadRecordingResolution();
			return;
		}

		config_t *config = obs_frontend_get_profile_config();
		if (!config)
			return;

		QString resolution = recordingResolution->itemData(index).toString();
		if (resolution == "same_canvas") {
			const auto baseWidth = config_get_uint(config, "Video", "BaseCX");
			const auto baseHeight = config_get_uint(config, "Video", "BaseCY");
			if (!baseWidth || !baseHeight)
				return;

			resolution = QString("%1x%2").arg(baseWidth).arg(baseHeight);
		}

		const QStringList parts = resolution.split('x');
		if (parts.size() != 2)
			return;

		bool widthOk = false;
		bool heightOk = false;
		const uint64_t width = parts[0].toULongLong(&widthOk);
		const uint64_t height = parts[1].toULongLong(&heightOk);
		if (!widthOk || !heightOk || !width || !height)
			return;

		config_set_uint(config, "Video", "OutputCX", width);
		config_set_uint(config, "Video", "OutputCY", height);
		config_save(config);
		refreshRecordingCodecAvailability();
		resetVideoIfIdle();
	}

	void setSpaceMode(bool spacesToUnderscores, bool removeSpaces)
	{
		if (spacesToUnderscores && removeSpaces) {
			if (sender() == ocrSpacesToUnderscores)
				removeSpaces = false;
			else
				spacesToUnderscores = false;
		}

		setBlocked(ocrSpacesToUnderscores, spacesToUnderscores);
		setBlocked(ocrRemoveSpaces, removeSpaces);
		updateSettings([spacesToUnderscores, removeSpaces](obs_data_t *settings) {
			obs_data_set_bool(settings, "ocr_spaces_to_underscores", spacesToUnderscores);
			obs_data_set_bool(settings, "ocr_remove_spaces", removeSpaces);
			obs_data_set_bool(settings, "ocr_enabled", true);
		});
	}

	void updateRecButton()
	{
		const bool recording = obs_frontend_recording_active();
		recButton->setText(recording ? "■" : "●");
		recButton->setProperty("recording", recording);
		recButton->style()->unpolish(recButton);
		recButton->style()->polish(recButton);
		recButton->setToolTip(recording ? trText("Stop") : trText("Rec"));
		if (recButtonLabel)
			recButtonLabel->setText(recording ? trText("Stop") : trText("Rec"));
	}

	void updateTopToggleButtons()
	{
		updateTopToggleButton(armedButton, armedButton && armedButton->isChecked() ? "✓" : "–",
				      armedButton && armedButton->isChecked() ? trText("Armed") : trText("Disarmed"),
				      armedButton && armedButton->isChecked() ? trText("Armed") : trText("Disarmed"),
				      armedButtonLabel);
		updateTopToggleButton(showOverlayButton,
				      showOverlayButton && showOverlayButton->isChecked() ? "👁" : "✕",
				      showOverlayButton && showOverlayButton->isChecked() ? trText("Hide")
											  : trText("Overlay"),
				      showOverlayButton && showOverlayButton->isChecked() ? trText("Overlay")
											  : trText("Hide"),
				      showOverlayButtonLabel);
	}

	void updateTopToggleButton(QPushButton *button, const QString &text, const QString &tooltip,
				   const QString &labelText, QLabel *label)
	{
		if (!button)
			return;
		button->setText(text);
		button->setToolTip(tooltip);
		button->setProperty("active", button->isChecked());
		button->style()->unpolish(button);
		button->style()->polish(button);
		if (label)
			label->setText(labelText);
	}

	void toggleRecording()
	{
		if (obs_frontend_recording_active())
			camera_tally_stop_recording(filter);
		else if (filter)
			camera_tally_start_recording_with_clip_name(filter);
		else
			obs_frontend_recording_start();
		updateRecButton();
	}

	void beginCenterSelection()
	{
		if (!filter)
			return;

		ensureOverlayVisible();
		beginPreviewPick(PickMode::DetectionCenter, trText("Click the detection point"));
		status->setText("Click the detection point directly in the main OBS preview.");
	}

	void requestAutoDetectCenter()
	{
		if (!filter)
			return;

		pickMode = PickMode::None;
		hidePreviewPickHint();
		removePreviewPickEventFilters();

			updateSettings([](obs_data_t *settings) {
				obs_data_set_bool(settings, "auto_detect_center_pending", true);
				obs_data_set_bool(settings, "show_overlay", true);
				obs_data_set_string(settings, "auto_detect_center_result", "waiting");
			});
		setBlocked(showOverlayButton, true);
		updateTopToggleButtons();
		setTemporaryStatus(trText("Automatic detection requested..."), 4);
		QTimer::singleShot(1800, this, [this]() { resolveAutoDetectCenterTimeout(); });
	}

	void resolveAutoDetectCenterTimeout()
	{
		if (!filter)
			return;

		obs_data_t *settings = obs_source_get_settings(filter);
		if (!settings)
			return;

		const char *raw = obs_data_get_string(settings, "auto_detect_center_result");
		const QString result = raw ? QString::fromUtf8(raw) : QString();
		if (result == "waiting") {
			obs_data_set_bool(settings, "auto_detect_center_pending", false);
			obs_data_set_string(settings, "auto_detect_center_result", "no_frame");
			obs_source_update(filter, settings);
		}

		obs_data_release(settings);
		refreshTarget();
	}

	void requestAutoDetectClipName()
	{
		if (!filter)
			return;

		pickMode = PickMode::None;
		hidePreviewPickHint();
		removePreviewPickEventFilters();

		updateSettings([](obs_data_t *settings) {
			obs_data_set_bool(settings, "auto_detect_clip_name_pending", true);
			obs_data_set_bool(settings, "show_overlay", true);
			obs_data_set_string(settings, "auto_detect_clip_name_result", "waiting");
		});
		setBlocked(showOverlayButton, true);
		updateTopToggleButtons();
		setTemporaryStatus(trText("Automatic clip name detection requested..."), 4);
		QTimer::singleShot(2200, this, [this]() { resolveAutoDetectClipNameTimeout(); });
	}

	void resolveAutoDetectClipNameTimeout()
	{
		if (!filter)
			return;

		obs_data_t *settings = obs_source_get_settings(filter);
		if (!settings)
			return;

		const char *raw = obs_data_get_string(settings, "auto_detect_clip_name_result");
		const QString result = raw ? QString::fromUtf8(raw) : QString();
		if (result == "waiting") {
			obs_data_set_bool(settings, "auto_detect_clip_name_pending", false);
			obs_data_set_string(settings, "auto_detect_clip_name_result", "no_frame");
			obs_source_update(filter, settings);
		}

		obs_data_release(settings);
		refreshTarget();
	}

	void requestClapperboard()
	{
		if (!filter)
			return;

		if (sectionPicker)
			sectionPicker->setCurrentIndex(5);
		if (clapperboardPreview && clapperboardPreviewPixmap.isNull())
			clapperboardPreview->setText(trText("No clap image yet"));
		if (clapperboardLoupe)
			clapperboardLoupe->hide();
		pickMode = PickMode::None;
		hidePreviewPickHint();
		removePreviewPickEventFilters();
		camera_tally_request_clapperboard(filter);
		setTemporaryStatus(trText("Clapperboard reading clap..."), 4);
		QTimer::singleShot(10000, this, [this]() { resolveClapperboardTimeout(); });
	}

	void requestMetadataAutoDetect()
	{
		if (!filter)
			return;

		pickMode = PickMode::None;
		hidePreviewPickHint();
		removePreviewPickEventFilters();
		camera_tally_request_metadata_auto_detect(filter);
		setTemporaryStatus(trText("Auto-detect metadata requested..."), 4);
		QTimer::singleShot(10000, this, [this]() { resolveClapperboardTimeout(); });
	}

	void resolveClapperboardTimeout()
	{
		if (!filter)
			return;

		obs_data_t *settings = obs_source_get_settings(filter);
		if (!settings)
			return;

		const char *raw = obs_data_get_string(settings, "clapperboard_result");
		const QString result = raw ? QString::fromUtf8(raw) : QString();
		if (result == "waiting") {
			obs_data_set_bool(settings, "clapperboard_pending", false);
			obs_data_set_string(settings, "clapperboard_result", "no_frame");
			obs_source_update(filter, settings);
		}

		obs_data_release(settings);
		refreshTarget();
	}

	void beginClipNameSelection()
	{
		if (!filter)
			return;

		ensureOverlayVisible();
		beginPreviewPick(PickMode::ClipName, trText("Click the center of the Clip Name"));
		status->setText("Click the center of the clip name text in the main OBS preview.");
	}

	void beginMetadataFieldSelection(MetadataFieldRow *row)
	{
		if (!filter || !row)
			return;

		activeMetadataPickRow = row;
		ensureOverlayVisible();
		beginPreviewPick(PickMode::MetadataField, trText("Click the metadata field center"));
		status->setText(trText("Click the metadata field center"));
	}

	void beginColorSelection()
	{
		if (!filter)
			return;

		beginPreviewPick(PickMode::Color, trText("Click the color to detect"));
		status->setText("Click the color to detect in the main OBS preview.");
	}

	void beginPreviewPick(PickMode mode, const QString &hintText)
	{
		pickMode = mode;
		installPreviewPickEventFilters();
		showPreviewPickHint(hintText);
	}

	void installPreviewPickEventFilters()
	{
		removePreviewPickEventFilters();

		auto addTarget = [this](QObject *target) {
			if (!target)
				return;
			for (const auto &existing : previewPickEventTargets) {
				if (existing == target)
					return;
			}
			target->installEventFilter(this);
			previewPickEventTargets.push_back(target);
		};

		addTarget(qApp);
		QWidget *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
		addTarget(mainWindow);
		QWidget *preview = mainPreviewWidget();
		addTarget(preview);
		if (preview) {
			const auto children = preview->findChildren<QObject *>();
			for (QObject *child : children)
				addTarget(child);
		}
	}

	void removePreviewPickEventFilters()
	{
		for (const auto &target : previewPickEventTargets) {
			if (target)
				target->removeEventFilter(this);
		}
		previewPickEventTargets.clear();
	}

	void ensureOverlayVisible()
	{
		setBool("show_overlay", true);
		setBlocked(showOverlayButton, true);
		updateTopToggleButtons();
	}

	void setDetectionCenterFromCursor(double x, double y)
	{
		if (!filter || pickMode != PickMode::DetectionCenter)
			return;

		pickMode = PickMode::None;
		x = std::clamp(x, 0.0, 1.0);
		y = std::clamp(y, 0.0, 1.0);
		updateSettings([x, y](obs_data_t *settings) {
			obs_data_set_double(settings, "center_x", x);
			obs_data_set_double(settings, "center_y", y);
		});
		setBlocked(centerX, displayValueForDoubleKey("center_x", x));
		setBlocked(centerY, displayValueForDoubleKey("center_y", y));
		syncDoubleSliders();
		status->setText(QString("Detection center selected: %1, %2\n%3")
					.arg(x, 0, 'f', 3)
					.arg(y, 0, 'f', 3)
					.arg(lastPickDebug));
		hidePreviewPickHint();
		removePreviewPickEventFilters();
	}

	void setClipNameCenterFromCursor(double x, double y)
	{
		if (!filter || pickMode != PickMode::ClipName)
			return;

		pickMode = PickMode::None;
		const double width = ocrWidth ? storedValueForDoubleKey("ocr_width", ocrWidth->value()) : 0.14;
		const double height = ocrHeight ? storedValueForDoubleKey("ocr_height", ocrHeight->value()) : 0.05;
			const double newX = std::clamp(x - width * 0.5, 0.0, std::max(0.0, 1.0 - width));
			const double newY = std::clamp(y - height * 0.5, 0.0, std::max(0.0, 1.0 - height));
		updateSettings([newX, newY](obs_data_t *settings) {
			obs_data_set_double(settings, "ocr_x", newX);
			obs_data_set_double(settings, "ocr_y", newY);
		});
		setBlocked(ocrX, newX);
		setBlocked(ocrY, newY);
		syncDoubleSliders();
		status->setText(QString("Clip name area centered: %1, %2\n%3")
					.arg(x, 0, 'f', 3)
					.arg(y, 0, 'f', 3)
					.arg(lastPickDebug));
		hidePreviewPickHint();
		removePreviewPickEventFilters();
	}

	void setMetadataFieldCenterFromCursor(double x, double y)
	{
		if (!filter || pickMode != PickMode::MetadataField || !activeMetadataPickRow)
			return;

		pickMode = PickMode::None;
		MetadataFieldRow *row = activeMetadataPickRow;
		activeMetadataPickRow = nullptr;
		if (row->x)
			row->x->setValue(std::clamp(x, 0.0, 1.0) * METADATA_CANVAS_WIDTH);
		if (row->y)
			row->y->setValue(std::clamp(y, 0.0, 1.0) * METADATA_CANVAS_HEIGHT);
		saveMetadataRowsToSettings();
		status->setText(QString("%1: %2, %3\n%4")
					.arg(trText("Metadata OCR area selected"))
					.arg(x, 0, 'f', 3)
					.arg(y, 0, 'f', 3)
					.arg(lastPickDebug));
		hidePreviewPickHint();
		if (qApp)
			qApp->removeEventFilter(this);
	}

	void setDetectionColorFromCursor(double x, double y)
	{
		if (!filter || pickMode != PickMode::Color)
			return;

		pickMode = PickMode::None;
		x = std::clamp(x, 0.0, 1.0);
		y = std::clamp(y, 0.0, 1.0);
		updateSettings([x, y](obs_data_t *settings) {
			obs_data_set_double(settings, "color_pick_x", x);
			obs_data_set_double(settings, "color_pick_y", y);
			obs_data_set_bool(settings, "color_pick_pending", true);
		});
		status->setText(QString("Color pick requested: %1, %2\n%3")
					.arg(x, 0, 'f', 3)
					.arg(y, 0, 'f', 3)
					.arg(lastPickDebug));
		hidePreviewPickHint();
		if (qApp)
			qApp->removeEventFilter(this);
	}

	static bool isPreviewWidgetClass(QWidget *widget, const QString &classNeedle)
	{
		const QString className = widget && widget->metaObject() ? widget->metaObject()->className() : "";
		return className.contains(classNeedle);
	}

	static QWidget *mainPreviewWidgetAt(const QPoint &globalPos)
	{
		QWidget *widget = QApplication::widgetAt(globalPos);
		while (widget) {
			if (isPreviewWidgetClass(widget, "OBSQTDisplay"))
				return widget;
			widget = widget->parentWidget();
		}
		return nullptr;
	}

	static QWidget *mainPreviewWidget()
	{
		auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
		if (!mainWindow)
			return nullptr;

		const auto widgets = mainWindow->findChildren<QWidget *>();
		for (QWidget *widget : widgets) {
			if (isPreviewWidgetClass(widget, "OBSQTDisplay") && widget->isVisible())
				return widget;
		}
		for (QWidget *widget : widgets) {
			if (isPreviewWidgetClass(widget, "OBSBasicPreview") && widget->isVisible())
				return widget;
		}
		for (QWidget *widget : widgets) {
			if (widget->objectName() == "preview")
				return widget;
		}
		for (QWidget *widget : widgets) {
			if (isPreviewWidgetClass(widget, "OBSQTDisplay"))
				return widget;
		}
		return nullptr;
	}

	static QScrollBar *mainWindowScrollBar(const char *objectName)
	{
		auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
		return mainWindow ? mainWindow->findChild<QScrollBar *>(objectName) : nullptr;
	}

	static QComboBox *mainWindowComboBox(const char *objectName)
	{
		auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
		return mainWindow ? mainWindow->findChild<QComboBox *>(objectName) : nullptr;
	}

	static double previewScaleFromLabel(double fallback)
	{
		auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
		QLabel *label = mainWindow ? mainWindow->findChild<QLabel *>("previewScalePercent") : nullptr;
		if (!label)
			return fallback;

		QString text = label->text().trimmed();
		if (text.endsWith('%'))
			text.chop(1);

		bool ok = false;
		const double percent = text.toDouble(&ok);
		return ok && percent > 0.0 ? percent / 100.0 : fallback;
	}

	void showPreviewPickHint(const QString &text)
	{
		QWidget *preview = mainPreviewWidget();
		if (!preview)
			return;

		hidePreviewPickHint();
		previewPickHint = new QLabel(text, preview);
		previewPickHint->setAlignment(Qt::AlignCenter);
		previewPickHint->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		previewPickHint->setStyleSheet(
			"background: rgba(10, 12, 18, 185); color: white; border: 2px solid rgba(255, 255, 255, 210);"
			"border-radius: 8px; padding: 10px 16px; font-size: 20px; font-weight: 800;");
		previewPickHint->adjustSize();
		const int width = std::min(previewPickHint->width() + 24, std::max(220, preview->width() - 40));
		const int height = previewPickHint->height() + 8;
		const int x = (preview->width() - width) / 2;
		const int y = std::max(18, preview->height() / 8);
			previewPickHint->setGeometry(x, y, width, height);
			previewPickHint->show();
			previewPickHint->raise();
			connect(previewPickHint, &QObject::destroyed, this, [this]() { previewPickHint = nullptr; });
			QPointer<QLabel> hint(previewPickHint);
			QTimer::singleShot(4500, this, [this, hint]() {
				if (hint && previewPickHint == hint)
					hidePreviewPickHint();
			});
		}

		void showPreviewResultToast(const QString &text, bool success)
		{
			QWidget *preview = mainPreviewWidget();
			if (!preview)
				return;

			if (previewResultToast && previewResultToastKey == text && previewResultToastSuccess == success) {
				updatePreviewResultToastText();
				previewResultToast->raise();
				return;
			}

			hidePreviewResultToast();
			previewResultToastKey = text;
			previewResultToastSuccess = success;
			previewResultToast = new QLabel(resultToastText(), preview);
			previewResultToast->setAlignment(Qt::AlignCenter);
			previewResultToast->setAttribute(Qt::WA_TransparentForMouseEvents, true);
			previewResultToast->setStyleSheet(
				QString("background: rgba(10, 12, 18, 210); color: white; border: 3px solid %1;"
					"border-radius: 10px; padding: 14px 24px; font-size: 28px; font-weight: 900;")
					.arg(success ? "rgba(40, 210, 95, 235)" : "rgba(235, 65, 65, 235)"));
			previewResultToast->adjustSize();
			const int width = std::min(previewResultToast->width() + 28, std::max(260, preview->width() - 60));
			const int height = previewResultToast->height() + 10;
			const int x = (preview->width() - width) / 2;
			const int y = (preview->height() - height) / 2;
			previewResultToast->setGeometry(x, y, width, height);
			previewResultToast->show();
			previewResultToast->raise();
			connect(previewResultToast, &QObject::destroyed, this, [this]() { previewResultToast = nullptr; });
			QPointer<QLabel> toast(previewResultToast);
			QTimer::singleShot(2500, this, [this, toast]() {
				if (toast && previewResultToast == toast)
					hidePreviewResultToast();
			});
		}

		QString resultToastText() const
		{
			if (previewResultToastKey.isEmpty())
				return QString();
			return QString("%1 %2")
				.arg(previewResultToastSuccess ? QStringLiteral("✓") : QStringLiteral("✕"),
				     trText(previewResultToastKey));
		}

		void updatePreviewResultToastText()
		{
			if (!previewResultToast)
				return;

			previewResultToast->setText(resultToastText());
			previewResultToast->adjustSize();
			QWidget *preview = mainPreviewWidget();
			if (!preview)
				return;

			const int width = std::min(previewResultToast->width() + 28, std::max(260, preview->width() - 60));
			const int height = previewResultToast->height() + 10;
			const int x = (preview->width() - width) / 2;
			const int y = (preview->height() - height) / 2;
			previewResultToast->setGeometry(x, y, width, height);
		}

		void hidePreviewResultToast()
		{
			if (!previewResultToast)
				return;
			QLabel *toast = previewResultToast;
			previewResultToast = nullptr;
			previewResultToastKey.clear();
			toast->hide();
			toast->setParent(nullptr);
			toast->deleteLater();
		}

		void hidePreviewPickHint()
		{
		if (!previewPickHint)
			return;
		QLabel *hint = previewPickHint;
		previewPickHint = nullptr;
		hint->hide();
		hint->setParent(nullptr);
		hint->deleteLater();
	}

	bool mainPreviewPosition(QMouseEvent *event, double &x, double &y)
	{
		const QPoint globalPos = event->globalPosition().toPoint();
		QWidget *preview = mainPreviewWidgetAt(globalPos);
		if (!preview)
			preview = mainPreviewWidget();
		if (!preview)
			return false;

		QPointF local = QPointF(preview->mapFromGlobal(globalPos));
		local += event->globalPosition() - QPointF(globalPos);
		if (local.x() < 0.0 || local.y() < 0.0 || local.x() >= preview->width() ||
		    local.y() >= preview->height())
			return false;

		obs_video_info videoInfo = {};
		if (!obs_get_video_info(&videoInfo) || videoInfo.base_width == 0 || videoInfo.base_height == 0)
			return false;

		const double pixelRatio = preview->devicePixelRatioF();
		const double widgetWidth = static_cast<double>(preview->width()) * pixelRatio;
		const double widgetHeight = static_cast<double>(preview->height()) * pixelRatio;
		if (widgetWidth <= 0.0 || widgetHeight <= 0.0 || pixelRatio <= 0.0)
			return false;

			const double baseWidth = static_cast<double>(videoInfo.base_width);
			const double baseHeight = static_cast<double>(videoInfo.base_height);
			const double availableWidth = widgetWidth - PREVIEW_EDGE_SIZE_LOCAL * 2.0;
			const double availableHeight = widgetHeight - PREVIEW_EDGE_SIZE_LOCAL * 2.0;
			if (availableWidth <= 0.0 || availableHeight <= 0.0)
				return false;

			double scale = 1.0;
			double offsetX = 0.0;
			double offsetY = 0.0;
			const double windowAspect = availableWidth / availableHeight;
			const double baseAspect = baseWidth / baseHeight;

			auto fitPreview = [&]() {
				double drawnWidth = availableWidth;
				double drawnHeight = availableHeight;
				if (windowAspect > baseAspect) {
					scale = availableHeight / baseHeight;
					drawnWidth = availableHeight * baseAspect;
				} else {
					scale = availableWidth / baseWidth;
					drawnHeight = availableWidth / baseAspect;
				}
				offsetX = availableWidth * 0.5 - drawnWidth * 0.5;
				offsetY = availableHeight * 0.5 - drawnHeight * 0.5;
			};

			QComboBox *scalingMode = mainWindowComboBox("previewScalingMode");
			QScrollBar *scrollX = mainWindowScrollBar("previewXScrollBar");
			QScrollBar *scrollY = mainWindowScrollBar("previewYScrollBar");
			const int scalingIndex = scalingMode ? scalingMode->currentIndex() : 0;
			const bool hasScroll = (scrollX && scrollX->isVisible() && scrollX->maximum() > scrollX->minimum()) ||
					       (scrollY && scrollY->isVisible() && scrollY->maximum() > scrollY->minimum());
			const bool fixedScale = scalingIndex != 0 || hasScroll;

			if (fixedScale) {
				if (scalingIndex == 1) {
					scale = 1.0;
				} else if (scalingIndex == 2 && videoInfo.output_width > 0) {
					scale = static_cast<double>(videoInfo.output_width) / baseWidth;
				} else {
					fitPreview();
					scale = previewScaleFromLabel(scale);
				}

				offsetX = (availableWidth - baseWidth * scale) * 0.5;
				offsetY = (availableHeight - baseHeight * scale) * 0.5;
				if (scrollX)
					offsetX -= static_cast<double>(scrollX->value());
				if (scrollY)
					offsetY -= static_cast<double>(scrollY->value());
			} else {
				fitPreview();
			}

			offsetX += PREVIEW_EDGE_SIZE_LOCAL;
			offsetY += PREVIEW_EDGE_SIZE_LOCAL;

		const double physicalX = local.x() * pixelRatio;
		const double physicalY = local.y() * pixelRatio;
		const double canvasX = (physicalX - offsetX) / scale;
		const double canvasY = (physicalY - offsetY) / scale;
		if (canvasX < 0.0 || canvasX > baseWidth || canvasY < 0.0 || canvasY > baseHeight) {
			status->setText("Click inside the displayed OBS preview image.");
			return false;
		}

		obs_sceneitem_t *item = findFilterSceneItem();
		if (!item) {
			status->setText("Select a source with the RecPilot filter in the current scene.");
			return false;
		}

		matrix4 transform;
		matrix4 inverse;
		vec3 canvasPos;
		vec3 sourcePos;
		vec3_set(&canvasPos, static_cast<float>(canvasX), static_cast<float>(canvasY), 0.0f);
		obs_sceneitem_get_draw_transform(item, &transform);
		matrix4_inv(&inverse, &transform);
		vec3_transform(&sourcePos, &canvasPos, &inverse);

		obs_source_t *itemSource = obs_sceneitem_get_source(item);
		const uint32_t sourceWidth = itemSource ? obs_source_get_base_width(itemSource) : 0;
		const uint32_t sourceHeight = itemSource ? obs_source_get_base_height(itemSource) : 0;
		const char *sourceName = itemSource ? obs_source_get_name(itemSource) : nullptr;
		if (sourceWidth == 0 || sourceHeight == 0)
			return false;

		if (sourcePos.x < 0.0f || sourcePos.x > static_cast<float>(sourceWidth) || sourcePos.y < 0.0f ||
		    sourcePos.y > static_cast<float>(sourceHeight)) {
			status->setText("Click inside the RecPilot source image.");
			return false;
		}

		x = std::clamp(static_cast<double>(sourcePos.x) / static_cast<double>(sourceWidth), 0.0, 1.0);
		y = std::clamp(static_cast<double>(sourcePos.y) / static_cast<double>(sourceHeight), 0.0, 1.0);
			lastPickDebug = QString("debug mode=%1 idx=%2 scale=%3 scroll=%4,%5 local=%6,%7 physical=%8,%9 canvas=%10,%11 source=%12 px=%13,%14 norm=%15,%16")
						.arg(fixedScale ? QString("fixed") : QString("fit"))
						.arg(scalingIndex)
						.arg(scale, 0, 'f', 4)
						.arg(scrollX ? scrollX->value() : 0)
						.arg(scrollY ? scrollY->value() : 0)
						.arg(local.x(), 0, 'f', 1)
						.arg(local.y(), 0, 'f', 1)
						.arg(physicalX, 0, 'f', 1)
						.arg(physicalY, 0, 'f', 1)
						.arg(canvasX, 0, 'f', 1)
					.arg(canvasY, 0, 'f', 1)
					.arg(sourceName ? QString::fromUtf8(sourceName) : QString("-"))
					.arg(sourcePos.x, 0, 'f', 1)
					.arg(sourcePos.y, 0, 'f', 1)
					.arg(x, 0, 'f', 4)
					.arg(y, 0, 'f', 4);
		blog(LOG_INFO, "RecPilot pick %s", lastPickDebug.toUtf8().constData());
		return true;
	}

	static QString presetPath()
	{
		char *path = obs_module_config_path("recpilot-presets.json");
		if (!path)
			return {};

		QString result = QString::fromUtf8(path);
		bfree(path);
		return result;
	}

	void reloadPresets()
	{
		customPresets = {};
		const QString path = presetPath();
		if (!path.isEmpty()) {
			QFile file(path);
			if (file.open(QIODevice::ReadOnly)) {
				const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
				if (doc.isObject())
					customPresets = doc.object().value("presets").toArray();
			}
		}

		QSignalBlocker blocker(presetPicker);
		presetPicker->clear();
		for (size_t i = 0; i < BUILTIN_PRESETS.size(); ++i)
			presetPicker->addItem(QString("Camera: %1").arg(BUILTIN_PRESETS[i].name),
					      QString("builtin:%1").arg(i));

		for (int i = 0; i < customPresets.size(); ++i) {
			const QJsonObject preset = customPresets[i].toObject();
			const QString name = preset.value("name").toString();
			if (!name.isEmpty())
				presetPicker->addItem(QString("Saved: %1").arg(name), QString("custom:%1").arg(i));
		}
	}

	void savePresetsToDisk()
	{
		const QString path = presetPath();
		if (path.isEmpty())
			return;

		QFileInfo info(path);
		info.absoluteDir().mkpath(".");

		QJsonObject root;
		root["presets"] = customPresets;
		QFile file(path);
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
			file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	}

	QJsonObject currentPresetValues(const QString &name) const
	{
		QJsonObject preset;
		preset["name"] = name;
		preset["center_x"] = storedValueForDoubleKey("center_x", centerX->value());
		preset["center_y"] = storedValueForDoubleKey("center_y", centerY->value());
		preset["radius"] = storedValueForDoubleKey("radius", radius->value());
		preset["ocr_x"] = ocrX->value();
		preset["ocr_y"] = ocrY->value();
		preset["ocr_width"] = storedValueForDoubleKey("ocr_width", ocrWidth->value());
		preset["ocr_height"] = storedValueForDoubleKey("ocr_height", ocrHeight->value());
		preset["metadata_fields"] = metadataPresetRowsJson();
		return preset;
	}

	static double displayValueForDoubleKey(const char *key, double storedValue)
	{
		if (std::strcmp(key, "center_x") == 0 || std::strcmp(key, "center_y") == 0)
			return std::clamp(storedValue * 100.0, 0.0, 100.0);
		if (std::strcmp(key, "ocr_width") == 0)
			return std::clamp(storedValue / clipNameWidthMax() * 100.0, 0.0, 100.0);
		if (std::strcmp(key, "ocr_height") == 0)
			return std::clamp(storedValue / clipNameHeightMax() * 100.0, 0.0, 100.0);
		if (std::strcmp(key, "radius") == 0)
			return radiusStoredToDisplay(storedValue);
		return storedValue;
	}

	static double storedValueForDoubleKey(const char *key, double displayValue)
	{
		if (std::strcmp(key, "center_x") == 0 || std::strcmp(key, "center_y") == 0)
			return std::clamp(displayValue / 100.0, 0.0, 1.0);
		if (std::strcmp(key, "ocr_width") == 0)
			return std::clamp(displayValue / 100.0, 0.0, 1.0) * clipNameWidthMax();
		if (std::strcmp(key, "ocr_height") == 0)
			return std::clamp(displayValue / 100.0, 0.0, 1.0) * clipNameHeightMax();
		if (std::strcmp(key, "radius") == 0)
			return radiusDisplayToStored(displayValue);
		return displayValue;
	}

	static double clipNameWidthMax()
	{
		return REC_PILOT_CLIP_NAME_WIDTH_MAX;
	}

	static double clipNameHeightMax()
	{
		return REC_PILOT_CLIP_NAME_HEIGHT_MAX;
	}

	static double radiusStoredToDisplay(double storedValue)
	{
		const double radiusMin = 0.002;
		const double radiusMid = 0.010;
		const double radiusMax = 0.040;
		storedValue = std::clamp(storedValue, radiusMin, radiusMax);
		if (storedValue <= radiusMid)
			return ((storedValue - radiusMin) / (radiusMid - radiusMin)) * 50.0;
		return 50.0 + ((storedValue - radiusMid) / (radiusMax - radiusMid)) * 50.0;
	}

	static double radiusDisplayToStored(double displayValue)
	{
		const double radiusMin = 0.002;
		const double radiusMid = 0.010;
		const double radiusMax = 0.040;
		displayValue = std::clamp(displayValue, 0.0, 100.0);
		if (displayValue <= 50.0)
			return radiusMin + (radiusMid - radiusMin) * (displayValue / 50.0);
		return radiusMid + (radiusMax - radiusMid) * ((displayValue - 50.0) / 50.0);
	}

	static QJsonObject builtinPresetObject(size_t index)
	{
		const DetectionPreset &preset = BUILTIN_PRESETS[index];
		QJsonObject object;
		object["name"] = preset.name;
		object["center_x"] = preset.center_x;
		object["center_y"] = preset.center_y;
		object["radius"] = preset.radius;
		object["ocr_x"] = preset.ocr_x;
		object["ocr_y"] = preset.ocr_y;
		object["ocr_width"] = preset.ocr_width;
		object["ocr_height"] = preset.ocr_height;
		object["metadata_fields"] = QJsonArray();
		return object;
	}

	QJsonObject selectedPreset() const
	{
		const QString id = presetPicker->currentData().toString();
		const QStringList parts = id.split(":");
		if (parts.size() != 2)
			return {};

		const int index = parts[1].toInt();
		if (parts[0] == "builtin" && index >= 0 && index < static_cast<int>(BUILTIN_PRESETS.size()))
			return builtinPresetObject(static_cast<size_t>(index));
		if (parts[0] == "custom" && index >= 0 && index < customPresets.size())
			return customPresets[index].toObject();
		return {};
	}

	void applyPreset(const QJsonObject &preset)
	{
		if (!filter || preset.isEmpty())
			return;

		const bool arriPreset = preset.value("name").toString().contains("ARRI", Qt::CaseInsensitive);
		const QByteArray metadataJson =
			QJsonDocument(preset.value("metadata_fields").toArray()).toJson(QJsonDocument::Compact);
		updateSettings([&preset, arriPreset, metadataJson](obs_data_t *settings) {
			obs_data_set_double(settings, "center_x", preset.value("center_x").toDouble());
			obs_data_set_double(settings, "center_y", preset.value("center_y").toDouble());
			obs_data_set_double(settings, "radius", preset.value("radius").toDouble());
			obs_data_set_double(settings, "ocr_x", preset.value("ocr_x").toDouble());
			obs_data_set_double(settings, "ocr_y", preset.value("ocr_y").toDouble());
			obs_data_set_double(settings, "ocr_width", preset.value("ocr_width").toDouble());
			obs_data_set_double(settings, "ocr_height", preset.value("ocr_height").toDouble());
			obs_data_set_bool(settings, "ocr_add_one", arriPreset);
			obs_data_set_string(settings, "metadata_fields_json", metadataJson.constData());
		});
		loadValues();
	}

	void loadSelectedPreset() { applyPreset(selectedPreset()); }

	void saveCurrentPreset()
	{
		if (!filter)
			return;

		bool ok = false;
		const QString name =
			QInputDialog::getText(this, "Save layout", "Preset name:", QLineEdit::Normal, "", &ok).trimmed();
		if (!ok || name.isEmpty())
			return;

		const QJsonObject preset = currentPresetValues(name);
		for (int i = 0; i < customPresets.size(); ++i) {
			if (customPresets[i].toObject().value("name").toString().compare(name, Qt::CaseInsensitive) ==
			    0) {
				customPresets[i] = preset;
				savePresetsToDisk();
				reloadPresets();
				return;
			}
		}

		customPresets.append(preset);
		savePresetsToDisk();
		reloadPresets();
	}

	void deleteSelectedPreset()
	{
		const QString id = presetPicker->currentData().toString();
		const QStringList parts = id.split(":");
		if (parts.size() != 2 || parts[0] != "custom")
			return;

		const int index = parts[1].toInt();
		if (index < 0 || index >= customPresets.size())
			return;

		customPresets.removeAt(index);
		savePresetsToDisk();
		reloadPresets();
	}

	static void setBlocked(QCheckBox *box, bool value)
	{
		if (!box)
			return;
		QSignalBlocker blocker(box);
		box->setChecked(value);
	}

	static void setBlocked(QPushButton *button, bool value)
	{
		if (!button)
			return;
		QSignalBlocker blocker(button);
		button->setChecked(value);
	}

	static void setBlocked(QDoubleSpinBox *box, double value)
	{
		if (!box)
			return;
		QSignalBlocker blocker(box);
		box->setValue(value);
	}

	static void setBlocked(QSpinBox *box, int value)
	{
		if (!box)
			return;
		QSignalBlocker blocker(box);
		box->setValue(value);
	}

	void updateSettings(const std::function<void(obs_data_t *)> &mutate)
	{
		if (!filter)
			return;

		obs_data_t *settings = obs_source_get_settings(filter);
		if (!settings)
			return;

		mutate(settings);
		obs_source_update(filter, settings);
		obs_data_release(settings);
	}

	void setBool(const char *key, bool value)
	{
		updateSettings([key, value](obs_data_t *settings) { obs_data_set_bool(settings, key, value); });
	}

	void setDouble(const char *key, double value)
	{
		if (strcmp(key, "ocr_width") == 0 || strcmp(key, "ocr_height") == 0) {
			updateSettings([this, key, value](obs_data_t *settings) {
				if (strcmp(key, "ocr_width") == 0) {
					const double oldX = obs_data_get_double(settings, "ocr_x");
					const double oldWidth = obs_data_get_double(settings, "ocr_width");
					const double center = oldX + oldWidth * 0.5;
					const double newX = std::clamp(center - value * 0.5, 0.0,
								       std::max(0.0, 1.0 - value));
					obs_data_set_double(settings, "ocr_x", newX);
					obs_data_set_double(settings, "ocr_width", value);
					setBlocked(ocrX, newX);
				} else {
					const double oldY = obs_data_get_double(settings, "ocr_y");
					const double oldHeight = obs_data_get_double(settings, "ocr_height");
					const double center = oldY + oldHeight * 0.5;
					const double newY = std::clamp(center - value * 0.5, 0.0,
								       std::max(0.0, 1.0 - value));
					obs_data_set_double(settings, "ocr_y", newY);
					obs_data_set_double(settings, "ocr_height", value);
					setBlocked(ocrY, newY);
				}
			});
			return;
		}
		updateSettings([key, value](obs_data_t *settings) { obs_data_set_double(settings, key, value); });
	}

	void syncDoubleSlider(QDoubleSpinBox *box)
	{
		auto it = doubleSliders.find(box);
		if (it == doubleSliders.end())
			return;

		const double min = box->minimum();
		const double max = box->maximum();
		const double value = box->value();
		const double ratio = max > min ? (value - min) / (max - min) : 0.0;
		QSignalBlocker blocker(it->second);
		it->second->setValue(static_cast<int>(std::clamp(ratio, 0.0, 1.0) * 1000.0));
	}

	void syncDoubleSliders()
	{
		for (const auto &entry : doubleSliders)
			syncDoubleSlider(entry.first);
	}

	void addDoubleControl(QVBoxLayout *layout, const char *labelText, QDoubleSpinBox *&box, const char *key,
			      double min, double max, double step, int decimals)
	{
		auto *row = new QGridLayout();
		row->setColumnStretch(0, 1);
		auto *label = new QLabel(labelText);
		tagText(label, labelText);
		box = new QDoubleSpinBox();
		box->setRange(min, max);
		box->setSingleStep(step);
		box->setDecimals(decimals);
		box->setKeyboardTracking(false);
		box->setMinimumWidth(82);
		if (std::strcmp(key, "center_x") == 0 || std::strcmp(key, "center_y") == 0 ||
		    std::strcmp(key, "radius") == 0 || std::strcmp(key, "ocr_width") == 0 ||
		    std::strcmp(key, "ocr_height") == 0)
			box->setSuffix(" %");
		auto *slider = new QSlider(Qt::Horizontal);
		slider->setRange(0, 1000);
		doubleSliders[box] = slider;

		row->addWidget(label, 0, 0, 1, 2);
		row->addWidget(slider, 1, 0);
		row->addWidget(box, 1, 1);
		layout->addLayout(row);

		connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
			[this, slider, key, min, max](double value) {
				QSignalBlocker blocker(slider);
				slider->setValue(static_cast<int>(((value - min) / (max - min)) * 1000.0));
				setDouble(key, storedValueForDoubleKey(key, value));
			});

		connect(slider, &QSlider::valueChanged, this, [this, box, key, min, max](int pos) {
			const double value = min + (max - min) * (static_cast<double>(pos) / 1000.0);
			QSignalBlocker blocker(box);
			box->setValue(value);
			setDouble(key, storedValueForDoubleKey(key, value));
		});
	}

	void addIntControl(QVBoxLayout *layout, const char *labelText, QSpinBox *&box, const char *key, int min,
			   int max)
	{
		auto *row = new QGridLayout();
		row->setColumnStretch(0, 1);
		auto *label = new QLabel(labelText);
		tagText(label, labelText);
		box = new QSpinBox();
		box->setRange(min, max);
		box->setKeyboardTracking(false);
		box->setMinimumWidth(82);
		auto *slider = new QSlider(Qt::Horizontal);
		slider->setRange(min, max);

		row->addWidget(label, 0, 0, 1, 2);
		row->addWidget(slider, 1, 0);
		row->addWidget(box, 1, 1);
		layout->addLayout(row);

		connect(box, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, slider, key](int value) {
			QSignalBlocker blocker(slider);
			slider->setValue(value);
			updateSettings([key, value](obs_data_t *settings) { obs_data_set_int(settings, key, value); });
		});

		connect(slider, &QSlider::valueChanged, this, [this, box, key](int value) {
			QSignalBlocker blocker(box);
			box->setValue(value);
			updateSettings([key, value](obs_data_t *settings) { obs_data_set_int(settings, key, value); });
		});
	}
};

QPointer<RecTriggerDock> dock = nullptr;

} // namespace

void create_rec_trigger_dock()
{
	if (dock)
		return;

	dock = new RecTriggerDock();
	QObject::connect(dock, &QObject::destroyed, []() { dock = nullptr; });
	const QByteArray dockTitle = QString::fromUtf8(APP_NAME).toUtf8();
	if (!obs_frontend_add_dock_by_id(DOCK_ID, dockTitle.constData(), dock)) {
		blog(LOG_WARNING, "Failed to add RecPilot dock");
		delete dock;
		dock = nullptr;
	}
}

void destroy_rec_trigger_dock()
{
	if (!dock)
		return;

	obs_frontend_remove_dock(DOCK_ID);
	dock = nullptr;
}
