#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""PySide6 desktop front end for the FilmViz Python bindings."""

from __future__ import annotations

import os
from pathlib import Path
import sys
import threading
import time
import traceback
from datetime import datetime


def _add_local_paths():
    script = Path(__file__).resolve()
    root = Path(
        os.environ.get(
            "FILMVIZ_PROJECT_ROOT",
            script.parent.parent)).resolve()

    for directory in (
        script.parent,
        root / "python",
        root / "build" / "Debug",
        root / "build" / "Release",
        root / "build" / "bin",
    ):
        if directory.exists():
            sys.path.insert(0, str(directory))

    prefixes = []
    configured_python = None
    environment_prefix = os.environ.get("FILMVIZ_DEPENDENCY_PREFIX")
    if environment_prefix:
        prefixes.append(Path(environment_prefix))

    cache = root / "build" / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(errors="ignore").splitlines():
            if line.startswith("CMAKE_PREFIX_PATH:") and "=" in line:
                prefixes.extend(Path(value) for value in line.split("=", 1)[1].split(";") if value)
            elif line.startswith("_Python3_EXECUTABLE:") and "=" in line:
                configured_python = Path(line.split("=", 1)[1])

    version = f"python{sys.version_info.major}.{sys.version_info.minor}"
    for prefix in prefixes:
        for directory in (prefix / "site-packages", prefix / "lib" / version / "site-packages"):
            if directory.exists():
                sys.path.insert(0, str(directory))

    return root, prefixes, configured_python


def _ensure_macos_qt_runtime(prefixes, configured_python):
    if sys.platform != "darwin":
        return

    debug_dependencies = any(str(prefix).endswith(".debug") for prefix in prefixes)
    release_dependencies = any(str(prefix).endswith(".release") for prefix in prefixes)

    if not debug_dependencies and not release_dependencies:
        return

    desired_suffix = "_debug" if debug_dependencies else None
    current_suffix = os.environ.get("DYLD_IMAGE_SUFFIX")

    if current_suffix == desired_suffix:
        return

    environment = os.environ.copy()
    if desired_suffix:
        environment["DYLD_IMAGE_SUFFIX"] = desired_suffix
    else:
        environment.pop("DYLD_IMAGE_SUFFIX", None)

    executable = configured_python or Path(sys.executable)
    if not executable.exists():
        raise SystemExit(f"Configured Python interpreter does not exist: {executable}")

    os.execve(
        str(executable),
        [str(executable), str(Path(__file__).resolve()), *sys.argv[1:]],
        environment,
    )


PROJECT_ROOT, DEPENDENCY_PREFIXES, CONFIGURED_PYTHON = _add_local_paths()
_ensure_macos_qt_runtime(DEPENDENCY_PREFIXES, CONFIGURED_PYTHON)

try:
    import filmviz_python as filmviz
    from PySide6.QtCore import QObject, QPointF, QRectF, QThread, QTimer, QUrl, Qt, Signal, Slot
    from PySide6.QtGui import QColor, QDesktopServices, QPainter, QPainterPath, QPen
    from PySide6.QtWidgets import (
        QApplication,
        QComboBox,
        QDoubleSpinBox,
        QFileDialog,
        QFormLayout,
        QGridLayout,
        QGroupBox,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QMainWindow,
        QMessageBox,
        QProgressBar,
        QPushButton,
        QSizePolicy,
        QSpinBox,
        QTabWidget,
        QVBoxLayout,
        QWidget,
    )
except ImportError as error:
    raise SystemExit(
        f"FilmViz Python application dependencies are unavailable: {error}\n"
        "Build with FILMVIZ_BUILD_PYTHON_APP=ON and ensure PySide6 is available."
    ) from error


class OperationWorker(QObject):
    progress = Signal(str, int, int)
    finished = Signal(str)
    failed = Signal(str)
    cancelled = Signal(str)

    def __init__(self, operation, success_message: str, cancel_event):
        super().__init__()
        self.operation = operation
        self.success_message = success_message
        self.cancel_event = cancel_event

    @Slot()
    def run(self):
        try:
            self.operation(
                self.progress.emit,
                self.cancel_event.is_set)
        except Exception:
            if self.cancel_event.is_set():
                self.cancelled.emit("Cancelled")
            else:
                self.failed.emit(traceback.format_exc())
        else:
            if self.cancel_event.is_set():
                self.cancelled.emit("Cancelled")
            else:
                self.finished.emit(self.success_message)


class PathRow(QWidget):
    def __init__(self, mode: str, initial: str = "", minimum_width: int = 0):
        super().__init__()
        self.mode = mode
        self.edit = QLineEdit(initial)
        self.edit.setMinimumWidth(minimum_width)
        self.edit.setSizePolicy(
            QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        button = QPushButton("Browse…")
        button.setMinimumWidth(110)
        button.setSizePolicy(
            QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        button.clicked.connect(self._browse)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.edit, 1)
        layout.addWidget(button, 0)

    @Slot()
    def _browse(self):
        current = self.edit.text() or str(PROJECT_ROOT)
        if self.mode == "directory":
            selected = QFileDialog.getExistingDirectory(self, "Select directory", current)
        elif self.mode == "input":
            selected, _ = QFileDialog.getOpenFileName(
                self, "Select input image", current,
                "Images (*.exr *.tif *.tiff *.png *.jpg *.jpeg);;All files (*)"
            )
        elif self.mode == "lut":
            selected, _ = QFileDialog.getSaveFileName(
                self, "Write LUT", current, "Cube LUT (*.cube)"
            )
        else:
            selected, _ = QFileDialog.getSaveFileName(
                self, "Write image", current,
                "TIFF (*.tif *.tiff);;OpenEXR (*.exr);;PNG (*.png);;All files (*)"
            )
        if selected:
            self.edit.setText(selected)

    def value(self) -> str:
        return self.edit.text().strip()


def _double(value, minimum, maximum, step=0.1, decimals=3):
    widget = QDoubleSpinBox()
    widget.setRange(minimum, maximum)
    widget.setSingleStep(step)
    widget.setDecimals(decimals)
    widget.setValue(value)
    return widget


def _read_curve_csv(
    filename: Path,
    x_column: str | None = None,
    y_columns: tuple[str, ...] | None = None,
):
    if not filename.is_file():
        return "", []

    rows = []
    with filename.open("r", encoding="utf-8-sig", errors="replace") as handle:
        header = None
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue

            fields = [field.strip() for field in line.split(",")]

            if header is None:
                header = fields
                continue

            values = {}
            for index, name in enumerate(header):
                if index >= len(fields) or not fields[index]:
                    values[name] = None
                    continue
                try:
                    values[name] = float(fields[index])
                except ValueError:
                    values[name] = None

            rows.append(values)

    if not rows or not header:
        return "", []

    x_name = x_column or header[0]
    if x_name not in header:
        return "", []

    names = (
        list(y_columns)
        if y_columns is not None
        else [name for name in header if name != x_name]
    )

    curves = []
    for name in names:
        if name not in header:
            continue
        points = [
            (row[x_name], row[name])
            for row in rows
            if row.get(x_name) is not None
            and row.get(name) is not None
        ]
        if points:
            curves.append((name, points))

    return x_name, curves


class CurvePlotWidget(QWidget):
    def __init__(self):
        super().__init__()
        self.setMinimumHeight(330)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)
        self._title = ""
        self._x_label = ""
        self._curves = []
        self._error = ""
        self._stop_axis = None

    def set_curves(
        self,
        title: str,
        x_label: str,
        curves,
        stop_axis=None,
    ):
        self._title = title
        self._x_label = x_label
        self._curves = curves
        self._stop_axis = stop_axis
        self._error = ""
        self.update()

    def set_error(self, message: str):
        self._title = ""
        self._x_label = ""
        self._curves = []
        self._stop_axis = None
        self._error = message
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        palette = self.palette()
        text_color = palette.color(self.foregroundRole())
        grid_color = palette.color(self.backgroundRole()).lighter(145)
        frame_color = palette.color(self.backgroundRole()).lighter(175)

        painter.fillRect(self.rect(), palette.color(self.backgroundRole()))

        if self._error:
            painter.setPen(text_color)
            painter.drawText(
                self.rect().adjusted(20, 20, -20, -20),
                Qt.AlignmentFlag.AlignCenter
                | Qt.TextFlag.TextWordWrap,
                self._error)
            return

        all_points = [
            point
            for _, points in self._curves
            for point in points
        ]

        if not all_points:
            painter.setPen(text_color)
            painter.drawText(
                self.rect(),
                Qt.AlignmentFlag.AlignCenter,
                "No curve data")
            return

        x_min = min(point[0] for point in all_points)
        x_max = max(point[0] for point in all_points)
        y_min = min(point[1] for point in all_points)
        y_max = max(point[1] for point in all_points)

        if abs(x_max - x_min) < 1e-12:
            x_max = x_min + 1.0
        if abs(y_max - y_min) < 1e-12:
            y_max = y_min + 1.0

        y_padding = 0.06 * (y_max - y_min)
        y_min -= y_padding
        y_max += y_padding

        left = 68.0
        right = 24.0
        top = 42.0
        bottom = 72.0 if self._stop_axis else 52.0
        plot = QRectF(
            left,
            top,
            max(1.0, self.width() - left - right),
            max(1.0, self.height() - top - bottom))

        painter.setPen(QPen(frame_color, 1.0))
        painter.drawRect(plot)

        painter.setPen(QPen(grid_color, 1.0))
        for index in range(1, 5):
            t = index / 5.0
            x = plot.left() + t * plot.width()
            y = plot.top() + t * plot.height()
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()))
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y))

        painter.setPen(text_color)
        painter.drawText(
            QRectF(0, 8, self.width(), 24),
            Qt.AlignmentFlag.AlignCenter,
            self._title)

        def map_point(x, y):
            px = plot.left() + (x - x_min) / (x_max - x_min) * plot.width()
            py = plot.bottom() - (y - y_min) / (y_max - y_min) * plot.height()
            return QPointF(px, py)

        colors = [
            QColor("#e05252"),
            QColor("#59b66b"),
            QColor("#5d87d7"),
            QColor("#d6a84f"),
            QColor("#b56bd4"),
            QColor("#5bb9bf"),
        ]

        for curve_index, (name, points) in enumerate(self._curves):
            if len(points) < 2:
                continue

            path = QPainterPath()
            path.moveTo(map_point(points[0][0], points[0][1]))
            for x, y in points[1:]:
                path.lineTo(map_point(x, y))

            painter.setPen(
                QPen(
                    colors[curve_index % len(colors)],
                    1.8))
            painter.drawPath(path)

        painter.setPen(text_color)
        painter.drawText(
            QRectF(plot.left(), plot.bottom() + 22, plot.width(), 20),
            Qt.AlignmentFlag.AlignCenter,
            self._x_label)

        if self._stop_axis:
            stop_min, stop_max, zero_x = self._stop_axis
            tick_count = int(round(stop_max - stop_min))
            log10_two = 0.3010299956639812

            secondary_color = QColor(text_color)
            secondary_color.setAlpha(150)

            painter.setPen(QPen(secondary_color, 1.0))
            for index in range(tick_count + 1):
                stop = stop_min + index
                stop_x = zero_x + stop * log10_two

                if stop_x < x_min - 1e-9 or stop_x > x_max + 1e-9:
                    continue

                x = map_point(stop_x, y_min).x()

                painter.drawLine(
                    QPointF(x, plot.bottom()),
                    QPointF(x, plot.bottom() + 5))

                if index % 2 == 0 or abs(stop) < 1e-9:
                    painter.setPen(secondary_color)
                    label = "0 stop" if abs(stop) < 1e-9 else f"{stop:+.0f}"
                    painter.drawText(
                        QRectF(x - 32, plot.bottom() + 42, 64, 18),
                        Qt.AlignmentFlag.AlignCenter,
                        label)

            if x_min <= zero_x <= x_max:
                zero_px = map_point(zero_x, y_min).x()
                zero_pen = QPen(secondary_color, 1.0)
                zero_pen.setStyle(Qt.PenStyle.DashLine)
                painter.setPen(zero_pen)
                painter.drawLine(
                    QPointF(zero_px, plot.top()),
                    QPointF(zero_px, plot.bottom()))

            painter.setPen(secondary_color)
            painter.drawText(
                QRectF(plot.left(), plot.bottom() + 56, plot.width(), 18),
                Qt.AlignmentFlag.AlignCenter,
                "camera stops")

        painter.drawText(
            QRectF(4, plot.top() - 8, left - 12, 20),
            Qt.AlignmentFlag.AlignRight,
            f"{y_max:.4g}")
        painter.drawText(
            QRectF(4, plot.bottom() - 12, left - 12, 20),
            Qt.AlignmentFlag.AlignRight,
            f"{y_min:.4g}")
        painter.drawText(
            QRectF(plot.left() - 16, plot.bottom() + 2, 60, 20),
            Qt.AlignmentFlag.AlignLeft,
            f"{x_min:.4g}")
        painter.drawText(
            QRectF(plot.right() - 44, plot.bottom() + 2, 60, 20),
            Qt.AlignmentFlag.AlignRight,
            f"{x_max:.4g}")

        legend_x = plot.left() + 8
        legend_y = plot.top() + 8
        for curve_index, (name, _) in enumerate(self._curves):
            painter.setPen(
                QPen(
                    colors[curve_index % len(colors)],
                    2.0))
            painter.drawLine(
                QPointF(legend_x, legend_y + 7),
                QPointF(legend_x + 18, legend_y + 7))
            painter.setPen(text_color)
            painter.drawText(
                QRectF(legend_x + 24, legend_y - 2, 220, 18),
                Qt.AlignmentFlag.AlignLeft
                | Qt.AlignmentFlag.AlignVCenter,
                name)
            legend_y += 20



class FilmVizWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("FilmViz — Experimental Spectral Film Processor")
        self.resize(820, 700)
        self._thread = None
        self._worker = None
        self._pending_output_image = None
        self._last_output_image = None
        self._cancel_event = None
        self._start_time = None
        self._current_stage = None
        self._stage_start_time = None
        self._last_logged_percent = -1
        self._log_path = PROJECT_ROOT / "build" / "filmviz_timings.log"

        central = QWidget()
        root_layout = QVBoxLayout(central)
        self.setCentralWidget(central)

        profiles = filmviz.profiles()
        common = QGroupBox("Pipeline")
        common_form = QFormLayout(common)
        common_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        self.resources = PathRow("directory", str(PROJECT_ROOT / "resources"))
        common_form.addRow("Resource directory", self.resources)

        self.input_profile = QComboBox()
        self.input_profile.addItems(profiles["input"])
        common_form.addRow("Input profile", self.input_profile)

        self.negative_profile = QComboBox()
        self.negative_profile.addItems(profiles["negative"])
        common_form.addRow("Negative", self.negative_profile)

        self.print_profile = QComboBox()
        self.print_profile.addItems(profiles["print"])
        common_form.addRow("Print", self.print_profile)

        self.output_profile = QComboBox()
        self.output_profile.addItems(profiles["output"])
        self.output_profile.setCurrentText("rec709-gamma24")
        common_form.addRow("Output profile", self.output_profile)

        self.exposure = _double(0.0, -10.0, 10.0, 0.25)
        self.push_pull = _double(0.0, -5.0, 5.0, 0.25)
        self.negative_bleach_bypass = _double(0.0, 0.0, 1.0, 0.05)
        self.print_bleach_bypass = _double(0.0, 0.0, 1.0, 0.05)
        self.printer_light_red = QSpinBox()
        self.printer_light_red.setRange(0, 50)
        self.printer_light_red.setValue(25)
        self.printer_light_green = QSpinBox()
        self.printer_light_green.setRange(0, 50)
        self.printer_light_green.setValue(25)
        self.printer_light_blue = QSpinBox()
        self.printer_light_blue.setRange(0, 50)
        self.printer_light_blue.setValue(25)
        self.middle_gray = _double(0.18, 0.001, 2.0, 0.01, 4)
        self.printer_temperature = _double(3200.0, 1000.0, 10000.0, 50.0, 0)
        self.lut_size = QSpinBox()
        self.lut_size.setRange(2, 129)
        self.lut_size.setValue(33)
        self.threads = QSpinBox()
        self.threads.setRange(0, 256)
        self.threads.setSpecialValueText("Auto")
        self.threads.setValue(0)

        controls = QWidget()
        controls_layout = QGridLayout(controls)
        controls_layout.setContentsMargins(0, 0, 0, 0)
        for row, (label, widget) in enumerate((
            ("Exposure stops", self.exposure),
            ("Push/pull stops", self.push_pull),
            ("Negative bypass", self.negative_bleach_bypass),
            ("Print bypass", self.print_bleach_bypass),
            ("Printer R light", self.printer_light_red),
            ("Printer G light", self.printer_light_green),
            ("Printer B light", self.printer_light_blue),
            ("Printer K", self.printer_temperature),
            ("Middle gray", self.middle_gray),
            ("LUT size", self.lut_size),
            ("Worker threads", self.threads),
        )):
            column = row % 2
            grid_row = row // 2
            controls_layout.addWidget(QLabel(label), grid_row, column * 2)
            controls_layout.addWidget(widget, grid_row, column * 2 + 1)
        common_form.addRow(controls)
        root_layout.addWidget(common)

        self.tabs = QTabWidget()
        root_layout.addWidget(self.tabs)

        image_tab = QWidget()
        image_form = QFormLayout(image_tab)
        image_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        default_image = PROJECT_ROOT / "resources" / "references" / "images" / "ARRI_Helen_John_ALEXA_Mini_LF_AWG3_LogC3.tif"
        self.input_image = PathRow("input", str(default_image))
        self.output_image = PathRow(
            "output",
            str(PROJECT_ROOT / "build" / "filmviz_output.tif"))
        image_form.addRow("Input image", self.input_image)
        image_form.addRow("Output image", self.output_image)
        self.negative_grain = _double(0.0, 0.0, 10.0)
        self.print_grain = _double(0.0, 0.0, 10.0)
        self.grain_size = _double(1.0, 1.0, 32.0, 0.25)
        self.grain_chroma = _double(1.0, 0.0, 4.0, 0.1)
        self.grain_seed = QSpinBox()
        self.grain_seed.setRange(0, 2_147_483_647)
        self.grain_seed.setValue(1)
        self.halation_strength = _double(0.0, 0.0, 1.0, 0.05)
        self.halation_radius = _double(12.0, 0.0, 200.0, 1.0, 1)
        self.halation_threshold = _double(0.7, 0.0, 4.0, 0.05, 3)
        image_form.addRow("Negative grain", self.negative_grain)
        image_form.addRow("Print grain", self.print_grain)
        image_form.addRow("Grain size (px)", self.grain_size)
        image_form.addRow("Grain chroma", self.grain_chroma)
        image_form.addRow("Grain seed", self.grain_seed)
        image_form.addRow("Halation", self.halation_strength)
        image_form.addRow("Halation radius (px)", self.halation_radius)
        image_form.addRow("Halation threshold", self.halation_threshold)
        self.convert_button = QPushButton("Convert image")
        self.convert_button.clicked.connect(self.convert_image)
        self.open_output_button = QPushButton("Open output")
        self.open_output_button.clicked.connect(self.open_output_image)
        self.open_output_button.setVisible(False)
        self.open_output_button.setEnabled(False)
        image_actions = QWidget()
        image_actions_layout = QHBoxLayout(image_actions)
        image_actions_layout.setContentsMargins(0, 0, 0, 0)
        image_actions_layout.addWidget(self.convert_button, 1)
        image_actions_layout.addWidget(self.open_output_button)
        image_form.addRow(image_actions)
        self.tabs.addTab(image_tab, "Convert Image")

        lut_tab = QWidget()
        lut_form = QFormLayout(lut_tab)
        lut_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        self.output_lut = PathRow("lut", str(PROJECT_ROOT / "build" / "filmviz.cube"))
        lut_form.addRow("Output LUT", self.output_lut)
        note = QLabel("LUTs are deterministic and do not contain image grain.")
        note.setWordWrap(True)
        lut_form.addRow(note)
        self.lut_button = QPushButton("Generate LUT")
        self.lut_button.clicked.connect(self.generate_lut)
        lut_form.addRow(self.lut_button)
        self.tabs.addTab(lut_tab, "Generate LUT")


        profiles_tab = QWidget()
        profiles_layout = QVBoxLayout(profiles_tab)

        profile_controls = QWidget()
        profile_controls_layout = QHBoxLayout(profile_controls)
        profile_controls_layout.setContentsMargins(0, 0, 0, 0)

        self.profile_family = QComboBox()
        self.profile_family.addItems((
            "Negative — Verita 200D",
            "Negative — Kodak VISION3 50D",
            "Print — Kodak 2383",
        ))
        self.profile_curve_type = QComboBox()

        profile_controls_layout.addWidget(QLabel("Profile"))
        profile_controls_layout.addWidget(self.profile_family)
        profile_controls_layout.addSpacing(18)
        profile_controls_layout.addWidget(QLabel("Curves"))
        profile_controls_layout.addWidget(self.profile_curve_type, 1)

        self.profile_plot = CurvePlotWidget()

        profiles_layout.addWidget(profile_controls)
        profiles_layout.addWidget(self.profile_plot, 1)

        self.profile_family.currentIndexChanged.connect(
            self._profile_family_changed)
        self.profile_curve_type.currentIndexChanged.connect(
            self._reload_profile_plot)
        self.resources.edit.editingFinished.connect(
            self._reload_profile_plot)

        self.tabs.addTab(profiles_tab, "Profiles")
        self._profile_family_changed()

        status = QWidget()
        status_layout = QHBoxLayout(status)
        status_layout.setContentsMargins(0, 0, 0, 0)

        self.stage = QLabel("Ready")
        self.elapsed = QLabel("Elapsed 00:00.0")
        self.open_log_button = QPushButton("Open log")
        self.open_log_button.clicked.connect(self.open_timing_log)
        self.open_log_button.setEnabled(self._log_path.is_file())
        self.cancel_button = QPushButton("Stop")
        self.cancel_button.setEnabled(False)
        self.cancel_button.clicked.connect(self.cancel_operation)

        status_layout.addWidget(self.stage, 1)
        status_layout.addWidget(self.elapsed)
        status_layout.addWidget(self.open_log_button)
        status_layout.addWidget(self.cancel_button)

        self.progress = QProgressBar()
        self.progress.setRange(0, 100)

        self.elapsed_timer = QTimer(self)
        self.elapsed_timer.setInterval(100)
        self.elapsed_timer.timeout.connect(self._update_elapsed)

        root_layout.addWidget(status)
        root_layout.addWidget(self.progress)


    @Slot()
    def _profile_family_changed(self):
        current_label = self.profile_curve_type.currentText()
        self.profile_curve_type.blockSignals(True)
        self.profile_curve_type.clear()

        if self.profile_family.currentIndex() == 0:
            entries = (
                ("Spectral sensitivity", "kodak_verita_200d_spectral_sensitivity_curves.csv"),
                ("Sensitometric curves", "kodak_verita_200d_sensitometric_curves.csv"),
                ("Spectral dye density", "kodak_verita_200d_spectral_dye_density_curves.csv"),
                ("Diffuse RMS granularity", "kodak_verita_200d_diffuse_rms_granularity_curves.csv"),
            )
        elif self.profile_family.currentIndex() == 1:
            entries = (
                ("Spectral sensitivity", "kodak_50d_spectral_sensitivity_curves.csv"),
                ("Sensitometric curves", "kodak_50d_sensitometric_curves.csv"),
                ("Spectral dye density", "kodak_50d_spectral_dye_density_curves.csv"),
                ("MTF", "kodak_50d_modulation_transfer_function_curves.csv"),
                ("Diffuse RMS granularity", "kodak_50d_diffuse_rms_granularity_curves.csv"),
            )
        else:
            entries = (
                ("Spectral sensitivity", "kodak_2383_spectral_sensitivity_curves.csv"),
                ("Sensitometric curves", "kodak_2383_sensitometric_curves.csv"),
                ("Spectral dye density", "kodak_2383_corrected_spectral_dye_density_curves.csv"),
                ("MTF", "kodak_2383_modulation_transfer_function_curves.csv"),
                ("Diffuse RMS granularity", "kodak_2383_diffuse_rms_granularity_curves.csv"),
            )

        for label, filename in entries:
            self.profile_curve_type.addItem(label, filename)

        if current_label:
            index = self.profile_curve_type.findText(current_label)
            if index >= 0:
                self.profile_curve_type.setCurrentIndex(index)
            elif self.profile_curve_type.count() > 0:
                self.profile_curve_type.setCurrentIndex(0)

        self.profile_curve_type.blockSignals(False)
        self._reload_profile_plot()

    @Slot()
    def _reload_profile_plot(self):
        filename = self.profile_curve_type.currentData()
        if not filename:
            self.profile_plot.set_error("No profile curve selected.")
            return

        profile_directory = (
            "verita_200d"
            if self.profile_family.currentIndex() == 0
            else "kodak_50d"
            if self.profile_family.currentIndex() == 1
            else "kodak_2383"
        )

        path = (
            Path(self.resources.value())
            / "profiles"
            / profile_directory
            / filename
        )

        x_column = None
        y_columns = None
        stop_axis = None

        if filename.endswith("_sensitometric_curves.csv"):
            if self.profile_family.currentIndex() in (0, 1):
                x_column = "log_exposure_lux_seconds"
                y_columns = (
                    "curve_high_density",
                    "curve_mid_density",
                    "curve_low_density",
                )
                # Both active camera-negative source tables carry camera stops
                # and LogE together with stop 0 anchored at -0.515 LogE.
                stop_axis = (-8.0, 8.0, -0.515)
            else:
                # Kodak 2383 sensitometry already uses log exposure as its
                # first column. Keep the file's native axis and plot all
                # remaining density channels. No photographic stop axis is
                # shown because there is no single calibrated 0-stop anchor
                # for the print stock.
                x_column = None
                y_columns = None

        x_label, curves = _read_curve_csv(
            path,
            x_column=x_column,
            y_columns=y_columns)

        if not curves:
            self.profile_plot.set_error(
                f"Could not load curve data:\n{path}")
            return

        title = (
            f"{self.profile_family.currentText()} — "
            f"{self.profile_curve_type.currentText()}"
        )

        self.profile_plot.set_curves(
            title,
            x_label,
            curves,
            stop_axis=stop_axis)


    def _common(self):
        return dict(
            resources=self.resources.value(),
            input=self.input_profile.currentText(),
            negative=self.negative_profile.currentText(),
            print=self.print_profile.currentText(),
            output=self.output_profile.currentText(),
            lut_size=self.lut_size.value(),
            exposure=self.exposure.value(),
            push_pull=self.push_pull.value(),
            negative_bleach_bypass=self.negative_bleach_bypass.value(),
            print_bleach_bypass=self.print_bleach_bypass.value(),
            printer_light_red=self.printer_light_red.value(),
            printer_light_green=self.printer_light_green.value(),
            printer_light_blue=self.printer_light_blue.value(),
            middle_gray=self.middle_gray.value(),
            printer_temperature=self.printer_temperature.value(),
            threads=self.threads.value(),
        )

    @Slot()
    def convert_image(self):
        arguments = self._common()
        arguments.update(
            input_filename=self.input_image.value(),
            output_filename=self.output_image.value(),
            negative_grain=self.negative_grain.value(),
            print_grain=self.print_grain.value(),
            grain_size=self.grain_size.value(),
            grain_chroma=self.grain_chroma.value(),
            grain_seed=self.grain_seed.value(),
            halation_strength=self.halation_strength.value(),
            halation_radius=self.halation_radius.value(),
            halation_threshold=self.halation_threshold.value(),
        )
        output = arguments["output_filename"]
        self._start(
            lambda progress, cancel: filmviz.process_image(
                **arguments,
                progress=progress,
                cancel=cancel),
            f"Wrote {output}",
            output_image=output,
            log_context=arguments,
        )

    @Slot()
    def generate_lut(self):
        arguments = self._common()
        arguments["output_filename"] = self.output_lut.value()
        output = arguments["output_filename"]
        self._start(
            lambda progress, cancel: filmviz.generate_lut(
                **arguments,
                progress=progress,
                cancel=cancel),
            f"Wrote {output}",
            log_context=arguments,
        )

    def _start(
        self,
        operation,
        success_message,
        output_image=None,
        log_context=None,
    ):
        if self._thread is not None:
            return

        self._pending_output_image = output_image
        self._cancel_event = threading.Event()
        self._start_time = time.monotonic()
        self._current_stage = None
        self._stage_start_time = None
        self._last_logged_percent = -1
        self._begin_timing_log(log_context or {})
        self.elapsed.setText("Elapsed 00:00.0")
        self.elapsed_timer.start()
        self.cancel_button.setEnabled(True)

        if output_image is not None:
            self.open_output_button.setVisible(False)
            self.open_output_button.setEnabled(False)

        self.convert_button.setEnabled(False)
        self.lut_button.setEnabled(False)
        self.progress.setValue(0)
        self.stage.setText("Starting…")

        thread = QThread(self)
        worker = OperationWorker(
            operation,
            success_message,
            self._cancel_event)
        worker.moveToThread(thread)
        thread.started.connect(worker.run)
        worker.progress.connect(self._progress)
        worker.finished.connect(self._finished)
        worker.failed.connect(self._failed)
        worker.cancelled.connect(self._cancelled)
        worker.finished.connect(thread.quit)
        worker.failed.connect(thread.quit)
        worker.cancelled.connect(thread.quit)
        thread.finished.connect(worker.deleteLater)
        thread.finished.connect(thread.deleteLater)
        thread.finished.connect(self._clear_worker)
        self._thread = thread
        self._worker = worker
        thread.start()

    @Slot()
    def cancel_operation(self):
        if self._cancel_event is None:
            return
        self._cancel_event.set()
        self.cancel_button.setEnabled(False)
        self.stage.setText("Stopping…")

    @Slot()
    def _update_elapsed(self):
        if self._start_time is None:
            return

        elapsed = max(0.0, time.monotonic() - self._start_time)
        minutes = int(elapsed // 60.0)
        seconds = elapsed - minutes * 60.0
        self.elapsed.setText(
            f"Elapsed {minutes:02d}:{seconds:04.1f}")

    def _stop_elapsed_timer(self):
        self._update_elapsed()
        self.elapsed_timer.stop()
        self.cancel_button.setEnabled(False)

    def _append_timing_log(self, message):
        try:
            self._log_path.parent.mkdir(parents=True, exist_ok=True)
            with self._log_path.open("a", encoding="utf-8") as handle:
                handle.write(message + "\n")
            self.open_log_button.setEnabled(True)
        except OSError:
            pass

    def _begin_timing_log(self, context):
        self._append_timing_log("")
        self._append_timing_log(
            "=" * 72)
        self._append_timing_log(
            f"Run started {datetime.now().isoformat(timespec='seconds')}")
        try:
            effective_threads = filmviz.effective_threads(4096)
            self._append_timing_log(
                f"effective_threads: {effective_threads}")
        except Exception:
            pass
        for key in sorted(context):
            self._append_timing_log(
                f"{key}: {context[key]}")

    def _finish_stage_log(self):
        if self._current_stage is None or self._stage_start_time is None:
            return
        duration = max(0.0, time.monotonic() - self._stage_start_time)
        self._append_timing_log(
            f"stage {self._current_stage}: {duration:.3f} s")
        self._current_stage = None
        self._stage_start_time = None
        self._last_logged_percent = -1

    @Slot()
    def open_timing_log(self):
        if not self._log_path.is_file():
            return
        if not QDesktopServices.openUrl(QUrl.fromLocalFile(str(self._log_path))):
            QMessageBox.warning(
                self,
                "Could not open timing log",
                f"Could not open:\n{self._log_path}")

    @Slot(str, int, int)
    def _progress(self, stage, completed, total):
        now = time.monotonic()

        if stage != self._current_stage:
            self._finish_stage_log()
            self._current_stage = stage
            self._stage_start_time = now
            self._last_logged_percent = -1
            self._append_timing_log(f"stage {stage}: started")

        percent = round(100 * completed / total) if total else 0
        self.stage.setText(stage)
        self.progress.setValue(percent)

        log_percent = (percent // 10) * 10
        if log_percent != self._last_logged_percent:
            elapsed = (
                now - self._stage_start_time
                if self._stage_start_time is not None
                else 0.0)
            self._append_timing_log(
                f"  {stage}: {percent}% at {elapsed:.3f} s")
            self._last_logged_percent = log_percent

    @Slot(str)
    def _finished(self, message):
        self._finish_stage_log()
        total = (
            max(0.0, time.monotonic() - self._start_time)
            if self._start_time is not None
            else 0.0)
        self._append_timing_log(f"run completed: {total:.3f} s")
        self._stop_elapsed_timer()
        self.progress.setValue(100)
        self.stage.setText(message)
        if self._pending_output_image is not None:
            path = Path(self._pending_output_image).expanduser().resolve()
            self._last_output_image = str(path)
            self.open_output_button.setEnabled(path.is_file())
            self.open_output_button.setVisible(True)

    @Slot(str)
    def _failed(self, message):
        self._finish_stage_log()
        total = (
            max(0.0, time.monotonic() - self._start_time)
            if self._start_time is not None
            else 0.0)
        self._append_timing_log(f"run failed after: {total:.3f} s")
        self._append_timing_log(message.rstrip())
        self._stop_elapsed_timer()
        self.stage.setText("Failed")
        if self._pending_output_image is not None:
            self.open_output_button.setVisible(False)
            self.open_output_button.setEnabled(False)
        QMessageBox.critical(self, "FilmViz failed", message)

    @Slot(str)
    def _cancelled(self, message):
        self._finish_stage_log()
        total = (
            max(0.0, time.monotonic() - self._start_time)
            if self._start_time is not None
            else 0.0)
        self._append_timing_log(f"run cancelled after: {total:.3f} s")
        self._stop_elapsed_timer()
        self.stage.setText(message)
        self.progress.setValue(0)
        if self._pending_output_image is not None:
            self.open_output_button.setVisible(False)
            self.open_output_button.setEnabled(False)

    @Slot()
    def open_output_image(self):
        if not self._last_output_image:
            return
        path = Path(self._last_output_image)
        if not path.is_file():
            QMessageBox.warning(
                self, "Output image unavailable", f"File not found:\n{path}")
            self.open_output_button.setEnabled(False)
            return
        if not QDesktopServices.openUrl(QUrl.fromLocalFile(str(path))):
            QMessageBox.warning(
                self, "Could not open output image", f"Could not open:\n{path}")

    @Slot()
    def _clear_worker(self):
        self._thread = None
        self._worker = None
        self._pending_output_image = None
        self._cancel_event = None
        self._start_time = None
        self._current_stage = None
        self._stage_start_time = None
        self._last_logged_percent = -1
        self._log_path = PROJECT_ROOT / "build" / "filmviz_timings.log"
        self.convert_button.setEnabled(True)
        self.lut_button.setEnabled(True)


def main() -> int:
    application = QApplication(sys.argv)
    window = FilmVizWindow()

    if os.environ.get("FILMVIZ_APP_SMOKE_TEST") == "1":
        print("FilmViz Python application smoke test passed")
        return 0

    window.show()
    return application.exec()


if __name__ == "__main__":
    raise SystemExit(main())
