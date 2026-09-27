#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""PySide6 desktop front end for the FilmViz Python bindings."""

from __future__ import annotations

import array
import csv
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
import traceback
from datetime import datetime


def _add_local_paths():
    script = Path(__file__).resolve()

    # The app is copied next to the matching Python extension module inside
    # the active CMake/Xcode configuration directory, e.g.
    #   build.release/Release/filmviz_app.py
    #   build.release/Release/filmviz_python.cpython-39-darwin.so
    #
    # Resolve the project root and dependency configuration from that runtime
    # layout instead of searching unrelated build trees such as repo/build/Debug.
    configured_root = os.environ.get("FILMVIZ_PROJECT_ROOT")
    if configured_root:
        root = Path(configured_root).resolve()
    else:
        root = script.parent.parent.parent.resolve()

    runtime_directory = script.parent
    build_root = runtime_directory.parent

    # Preserve the runtime directory as the highest-priority import location so
    # a Release launcher always loads the Release binding beside the app.
    for directory in reversed((
        runtime_directory,
        root / "python",
    )):
        if directory.exists():
            sys.path.insert(0, str(directory))

    prefixes = []
    configured_python = None

    environment_prefix = os.environ.get("FILMVIZ_DEPENDENCY_PREFIX")
    if environment_prefix:
        prefixes.append(Path(environment_prefix))

    # Use the CMake cache belonging to this exact build tree. In the normal
    # Release layout this is build.release/CMakeCache.txt, which carries the
    # matching Release OpenImageIO/Imath/PySide dependency prefixes.
    cache = build_root / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(errors="ignore").splitlines():
            if line.startswith("CMAKE_PREFIX_PATH:") and "=" in line:
                prefixes.extend(
                    Path(value)
                    for value in line.split("=", 1)[1].split(";")
                    if value)
            elif line.startswith("_Python3_EXECUTABLE:") and "=" in line:
                configured_python = Path(line.split("=", 1)[1])

    version = f"python{sys.version_info.major}.{sys.version_info.minor}"
    for prefix in prefixes:
        for directory in (
            prefix / "site-packages",
            prefix / "lib" / version / "site-packages",
        ):
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

from filmviz_memory import create_memory_log

try:
    import filmviz_python as filmviz

    try:
        import OpenImageIO as oiio
    except ImportError:
        oiio = None

    try:
        import numpy as np
    except ImportError:
        np = None

    from PySide6.QtCore import QSettings, QEvent, QMimeData, QObject, QPoint, QPointF, QRect, QRectF, QSize, QThread, QTimer, QUrl, Qt, Signal, Slot
    from PySide6.QtGui import (
        QColor,
        QColorSpace,
        QAction,
        QDesktopServices,
        QIcon,
        QImage,
        QKeySequence,
        QShortcut,
        QFontDatabase,
        QPainter,
        QPainterPath,
        QPen,
        QPixmap,
        QRegion,
        QSurfaceFormat,
    )
    from PySide6.QtWidgets import (
        QApplication,
        QCheckBox,
        QComboBox,
        QDoubleSpinBox,
        QFileDialog,
        QFormLayout,
        QGridLayout,
        QGroupBox,
        QHBoxLayout,
        QLabel,
        QInputDialog,
        QLineEdit,
        QMainWindow,
        QMenu,
        QMessageBox,
        QProgressBar,
        QPushButton,
        QPlainTextEdit,
        QScrollArea,
        QSizePolicy,
        QSlider,
        QSpinBox,
        QSplitter,
        QStackedWidget,
        QStyle,
        QTabWidget,
        QVBoxLayout,
        QWidget,
    )
except ImportError as error:
    raise SystemExit(
        f"FilmViz Python application dependencies are unavailable: {error}\n"
        "Build with FILMVIZ_BUILD_PYTHON_APP=ON and ensure PySide6 is available."
    ) from error



def _rec709_gamma_color_space(gamma: float):
    # Rec.709 and sRGB use the same RGB primaries / D65 white point.
    # Use an explicit gamma transfer function here because the FilmViz
    # display selector intentionally distinguishes Gamma 2.2 and 2.4.
    color_space = QColorSpace(
        QColorSpace.Primaries.SRgb,
        QColorSpace.TransferFunction.Gamma,
        gamma)
    color_space.setDescription(f"Rec.709 Gamma {gamma:g}")
    return color_space


def _display_color_space(name: str):
    if name == "rec709-gamma22":
        return _rec709_gamma_color_space(2.2)
    if name == "srgb":
        return QColorSpace(QColorSpace.NamedColorSpace.SRgb)
    return _rec709_gamma_color_space(2.4)


APP_COLOR_SPACE = _display_color_space("rec709-gamma24")




def _read_scope_rgb(filename: str):
    # Scope analysis must use the original output pixels, not the resized
    # RGB888 preview. Prefer a native FilmViz float reader if the binding
    # exposes one; otherwise use the OpenImageIO Python bindings from the
    # configured FilmViz dependency prefix.
    native_reader = getattr(filmviz, "read_image_scope", None)
    if native_reader is not None:
        image = native_reader(filename)
        width = int(image["width"])
        height = int(image["height"])
        rgb = image["rgb"]
        return width, height, rgb

    if oiio is None:
        raise RuntimeError(
            "High-precision scope analysis requires either "
            "filmviz.read_image_scope() or the OpenImageIO Python bindings. "
            "The scopes will not fall back to the 8-bit preview.")

    image = oiio.ImageBuf(filename)
    if image.has_error:
        raise RuntimeError(image.geterror())

    spec = image.spec()
    if spec.nchannels < 3:
        raise RuntimeError(
            f"Scope source must contain at least 3 channels: {filename}")

    roi = oiio.ROI(
        0, spec.width,
        0, spec.height,
        0, 1,
        0, 3)
    pixels = image.get_pixels(oiio.FLOAT, roi)
    if pixels is None:
        error = image.geterror()
        raise RuntimeError(
            error or f"Could not read high-precision scope pixels: {filename}")

    # OIIO normally returns an HxWx3 numpy array here. Keep it as a flat
    # float sequence without quantizing so the scope widgets see the actual
    # output values, including values below 0 or above 1.
    try:
        rgb = pixels.reshape(-1)
    except AttributeError:
        rgb = [
            component
            for row in pixels
            for pixel in row
            for component in pixel
        ]

    return int(spec.width), int(spec.height), rgb


def _scope_rgb_at(width: int, height: int, rgb, u: float, v: float):
    if width <= 0 or height <= 0 or rgb is None:
        return None

    x = min(width - 1, max(0, int(u * width)))
    y = min(height - 1, max(0, int(v * height)))
    offset = (y * width + x) * 3
    return (
        float(rgb[offset]),
        float(rgb[offset + 1]),
        float(rgb[offset + 2]),
    )


class ColorResponsePlot(QWidget):
    """Diagnostic slice of the C++ dye model, independent of the image preview."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(210)
        self._response = {}

    def set_response(self, response):
        self._response = response
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), self.palette().window())
        center = QPointF(self.width() * 0.5, self.height() * 0.48)
        scale = min(self.width(), self.height() - 45) * 0.85
        for key, color in (("bypass", QColor("#777777")), ("standard", QColor("#6da8cd")), ("tuned", QColor("#efa756"))):
            points = self._response.get(key, [])
            path = QPainterPath()
            for i, point in enumerate(points):
                q = QPointF(center.x() + point[0] * scale, center.y() - point[1] * scale)
                if i == 0:
                    path.moveTo(q)
                else:
                    path.lineTo(q)
            painter.setPen(color)
            painter.drawPath(path)
        painter.setPen(self.palette().text().color())
        painter.drawText(5, self.height() - 28, "Dye chroma: gray bypass / blue standard / amber tuned")
        painter.drawText(5, self.height() - 10, "Slice: neutral density 1.0, chroma RMS 0.2")


class ColorProfileSelector(QObject):
    """Two linked controls retaining the canonical profile as their value."""
    currentIndexChanged = Signal(int)

    def __init__(self, entries, parent=None):
        super().__init__(parent)
        self.color_space = QComboBox()
        self.transfer_function = QComboBox()
        self._entries = []
        self.color_space.currentIndexChanged.connect(self._space_changed)
        self.transfer_function.currentIndexChanged.connect(self._transfer_changed)
        self.setProfiles(entries)

    def currentText(self):
        return self.transfer_function.currentData() or ""

    def setProfiles(self, entries):
        selected = self.currentText()
        self._entries = [dict(entry) for entry in entries]
        blocked = self.blockSignals(True)
        self.color_space.blockSignals(True)
        self.color_space.clear()
        self.color_space.addItems(list(dict.fromkeys(e["color_space"] for e in self._entries)))
        self.color_space.blockSignals(False)
        self._space_changed()
        self.setCurrentText(selected)
        self.blockSignals(blocked)
        self._transfer_changed()

    def _space_changed(self, *_):
        previous = self.transfer_function.currentText()
        self.transfer_function.blockSignals(True)
        self.transfer_function.clear()
        for entry in self._entries:
            if entry["color_space"] == self.color_space.currentText():
                self.transfer_function.addItem(entry["transfer_function"], entry["profile"])
        index = self.transfer_function.findText(previous)
        self.transfer_function.setCurrentIndex(max(0, index))
        self.transfer_function.blockSignals(False)
        self._transfer_changed()

    def _transfer_changed(self, *_):
        self.currentIndexChanged.emit(next(
            (i for i, e in enumerate(self._entries) if e["profile"] == self.currentText()), -1))

    def setCurrentIndex(self, index):
        if 0 <= index < len(self._entries):
            self.setCurrentText(self._entries[index]["profile"])

    def setCurrentText(self, profile):
        entry = next((e for e in self._entries if e["profile"] == profile), None)
        if entry is None:
            return
        blocked = self.blockSignals(True)
        self.color_space.blockSignals(True)
        self.color_space.setCurrentText(entry["color_space"])
        self.color_space.blockSignals(False)
        self._space_changed()
        self.transfer_function.setCurrentIndex(self.transfer_function.findData(profile))
        self.blockSignals(blocked)
        self._transfer_changed()


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


class MetalPreviewBridge(QObject):
    finished = Signal(int, object)
    failed = Signal(int, str)


class PathRow(QWidget):
    def __init__(self, mode: str, initial: str = "", minimum_width: int = 0):
        super().__init__()
        self.mode = mode
        self.edit = QLineEdit(initial)
        if minimum_width > 0:
            self.edit.setMinimumWidth(min(120, minimum_width))
        self.edit.setSizePolicy(
            QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

        button = QPushButton()
        button.setIcon(
            self.style().standardIcon(
                QStyle.StandardPixmap.SP_DialogOpenButton))
        button.setToolTip("Browse")
        button.setFixedSize(30, 24)
        button.setSizePolicy(
            QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        button.clicked.connect(self._browse)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)
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
                "Images (*.exr *.dpx *.tif *.tiff *.png *.jpg *.jpeg);;All files (*)"
            )
        elif self.mode == "lut":
            selected, _ = QFileDialog.getSaveFileName(
                self, "Write LUT", current, "Cube LUT (*.cube)"
            )
        else:
            selected, _ = QFileDialog.getSaveFileName(
                self, "Write image", current,
                "TIFF (*.tif *.tiff);;OpenEXR (*.exr);;DPX (*.dpx);;PNG (*.png);;All files (*)"
            )
        if selected:
            self.edit.setText(selected)
            self.edit.editingFinished.emit()

    def value(self) -> str:
        return self.edit.text().strip()


class _DragSpinBoxMixin:
    """Horizontal scrub interaction shared by all FilmViz spin boxes.

    Normal interaction is click/drag left-right to change the value.
    Double-click switches the embedded line edit into text-edit mode.
    """

    _pixels_per_step = 6.0

    def _init_drag_spinbox(self):
        self._drag_active = False
        self._drag_start_x = 0.0
        self._drag_start_value = 0.0
        self._text_editing = False
        editor = self.lineEdit()
        editor.setReadOnly(True)
        editor.setCursor(Qt.CursorShape.SizeHorCursor)
        editor.installEventFilter(self)
        editor.editingFinished.connect(self._finish_text_edit)

    def _finish_text_edit(self):
        if self._text_editing:
            self.interpretText()
        self._text_editing = False
        editor = self.lineEdit()
        editor.setReadOnly(True)
        editor.setCursor(Qt.CursorShape.SizeHorCursor)
        self.clearFocus()

    def _begin_text_edit(self):
        if self.isReadOnly():
            return
        self._drag_active = False
        self._text_editing = True
        editor = self.lineEdit()
        editor.setReadOnly(False)
        editor.setCursor(Qt.CursorShape.IBeamCursor)
        editor.setFocus(Qt.FocusReason.MouseFocusReason)
        editor.selectAll()

    def _scrub_value(self, delta_x):
        if self.isReadOnly():
            return
        steps = float(delta_x) / self._pixels_per_step
        value = self._drag_start_value + steps * float(self.singleStep())
        if isinstance(self, QSpinBox):
            value = int(round(value))
        self.setValue(value)

    def eventFilter(self, watched, event):
        if watched is self.lineEdit():
            event_type = event.type()

            if event_type == QEvent.Type.Enter and not self._text_editing:
                watched.setCursor(Qt.CursorShape.SizeHorCursor)

            elif event_type == QEvent.Type.MouseButtonDblClick:
                if event.button() == Qt.MouseButton.LeftButton and not self.isReadOnly():
                    self._begin_text_edit()
                    event.accept()
                    return True

            elif event_type == QEvent.Type.MouseButtonPress:
                if (event.button() == Qt.MouseButton.LeftButton
                        and not self._text_editing
                        and not self.isReadOnly()):
                    self._drag_active = True
                    self._drag_start_x = float(event.globalPosition().x())
                    self._drag_start_value = float(self.value())
                    watched.setCursor(Qt.CursorShape.SizeHorCursor)
                    self.setFocus(Qt.FocusReason.MouseFocusReason)
                    event.accept()
                    return True

            elif event_type == QEvent.Type.MouseMove:
                if self._drag_active:
                    self._scrub_value(float(event.globalPosition().x()) - self._drag_start_x)
                    event.accept()
                    return True

            elif event_type == QEvent.Type.MouseButtonRelease:
                if self._drag_active and event.button() == Qt.MouseButton.LeftButton:
                    self._drag_active = False
                    event.accept()
                    return True

            elif event_type == QEvent.Type.FocusOut and self._text_editing:
                self._finish_text_edit()

        return super().eventFilter(watched, event)


class DragDoubleSpinBox(_DragSpinBoxMixin, QDoubleSpinBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._init_drag_spinbox()


class DragSpinBox(_DragSpinBoxMixin, QSpinBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._init_drag_spinbox()


def _double(value, minimum, maximum, step=0.1, decimals=3):
    widget = DragDoubleSpinBox()
    widget.setRange(minimum, maximum)
    widget.setSingleStep(step)
    widget.setDecimals(decimals)
    widget.setValue(value)
    return widget


def _reset_button():
    button = QPushButton("Reset")
    button.setFixedWidth(64)
    button.setToolTip("Reset this group to its defaults")
    return button




def _reference_image_icon(size: int = 18):
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.GlobalColor.transparent)

    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    pen = QPen(QColor(190, 190, 190), 1.4)
    painter.setPen(pen)
    painter.setBrush(Qt.BrushStyle.NoBrush)

    frame = QRectF(2.0, 3.0, size - 4.0, size - 6.0)
    painter.drawRoundedRect(frame, 2.0, 2.0)

    painter.drawEllipse(QPointF(size * 0.68, size * 0.35), 1.5, 1.5)
    painter.drawLine(
        QPointF(size * 0.22, size * 0.72),
        QPointF(size * 0.43, size * 0.50))
    painter.drawLine(
        QPointF(size * 0.43, size * 0.50),
        QPointF(size * 0.57, size * 0.63))
    painter.drawLine(
        QPointF(size * 0.57, size * 0.63),
        QPointF(size * 0.74, size * 0.46))
    painter.drawLine(
        QPointF(size * 0.74, size * 0.46),
        QPointF(size * 0.86, size * 0.60))
    painter.end()

    return QIcon(pixmap)


def _trash_icon(size: int = 18):
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.GlobalColor.transparent)

    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    painter.setPen(QPen(QColor(190, 190, 190), 1.4))
    painter.setBrush(Qt.BrushStyle.NoBrush)

    left = size * 0.30
    right = size * 0.70
    top = size * 0.34
    bottom = size * 0.82
    painter.drawRoundedRect(
        QRectF(left, top, right - left, bottom - top),
        1.5,
        1.5)
    painter.drawLine(
        QPointF(size * 0.24, size * 0.28),
        QPointF(size * 0.76, size * 0.28))
    painter.drawLine(
        QPointF(size * 0.42, size * 0.20),
        QPointF(size * 0.58, size * 0.20))
    painter.drawLine(
        QPointF(size * 0.44, size * 0.43),
        QPointF(size * 0.44, size * 0.72))
    painter.drawLine(
        QPointF(size * 0.56, size * 0.43),
        QPointF(size * 0.56, size * 0.72))
    painter.end()

    return QIcon(pixmap)

def _copy_icon(size: int = 18):
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.GlobalColor.transparent)

    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    painter.setPen(QPen(QColor(190, 190, 190), 1.4))
    painter.setBrush(Qt.BrushStyle.NoBrush)

    offset = max(2.0, size * 0.18)
    box = max(6.0, size * 0.55)
    painter.drawRoundedRect(
        QRectF(offset + 2.0, offset, box, box),
        1.5,
        1.5)
    painter.drawRoundedRect(
        QRectF(offset, offset + 2.0, box, box),
        1.5,
        1.5)
    painter.end()

    return QIcon(pixmap)


def _probe_marker_icon(size: int = 14):
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.GlobalColor.transparent)

    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    painter.setPen(QPen(QColor(245, 210, 70), 1.0))

    center = QPointF(size * 0.5, size * 0.5)
    radius = max(2.0, size * 0.22)
    painter.drawEllipse(center, radius, radius)
    painter.drawLine(
        QPointF(center.x() - radius - 2.0, center.y()),
        QPointF(center.x() + radius + 2.0, center.y()))
    painter.drawLine(
        QPointF(center.x(), center.y() - radius - 2.0),
        QPointF(center.x(), center.y() + radius + 2.0))
    painter.end()

    return QIcon(pixmap)


class SliderSpinRow(QWidget):
    def __init__(self, spinbox, slider_width: int = 150):
        super().__init__()

        self.spinbox = spinbox
        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setMinimumWidth(slider_width)
        self.slider.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Fixed)

        if isinstance(spinbox, QDoubleSpinBox):
            step = max(1e-9, float(spinbox.singleStep()))
            self._scale = 1.0 / step
            minimum = int(round(spinbox.minimum() * self._scale))
            maximum = int(round(spinbox.maximum() * self._scale))
            value = int(round(spinbox.value() * self._scale))
        else:
            self._scale = 1.0
            minimum = int(spinbox.minimum())
            maximum = int(spinbox.maximum())
            value = int(spinbox.value())

        self.slider.setRange(minimum, maximum)
        self.slider.setValue(value)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(5)
        layout.addWidget(self.slider, 1)
        layout.addWidget(spinbox, 0)

        self.slider.valueChanged.connect(
            self._slider_changed)
        spinbox.valueChanged.connect(
            self._spinbox_changed)

    def _slider_changed(self, value: int):
        target = value / self._scale
        if isinstance(self.spinbox, QSpinBox):
            target = int(round(target))

        if self.spinbox.value() != target:
            self.spinbox.blockSignals(True)
            self.spinbox.setValue(target)
            self.spinbox.blockSignals(False)
            self.spinbox.valueChanged.emit(self.spinbox.value())

    def _spinbox_changed(self, value):
        slider_value = int(round(float(value) * self._scale))
        if self.slider.value() != slider_value:
            self.slider.blockSignals(True)
            self.slider.setValue(slider_value)
            self.slider.blockSignals(False)


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
    pointSelected = Signal(int, int, str, float, float)
    pointChanged = Signal(int, int, str, float, float)

    def __init__(self):
        super().__init__()
        self.setMinimumHeight(330)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)
        self.setMouseTracking(True)
        self._title = ""
        self._x_label = ""
        self._curves = []
        self._original_curves = []
        self._error = ""
        self._stop_axis = None
        self._selected_curve = -1
        self._selected_point = -1
        self._dragging = False
        self._plot_rect = QRectF()
        self._x_min = 0.0
        self._x_max = 1.0
        self._y_min = 0.0
        self._y_max = 1.0
        self._edit_spacing_nm = 0.0

    def set_curves(
        self,
        title: str,
        x_label: str,
        curves,
        stop_axis=None,
        original_curves=None,
    ):
        self._title = title
        self._x_label = x_label
        self._curves = [
            (name, [(float(x), float(y)) for x, y in points])
            for name, points in curves
        ]
        source_original = original_curves if original_curves is not None else curves
        self._original_curves = [
            (name, [(float(x), float(y)) for x, y in points])
            for name, points in source_original
        ]
        self._stop_axis = stop_axis
        self._error = ""
        self._selected_curve = -1
        self._selected_point = -1
        self._dragging = False
        self.update()

    def set_error(self, message: str):
        self._title = ""
        self._x_label = ""
        self._curves = []
        self._original_curves = []
        self._stop_axis = None
        self._error = message
        self._selected_curve = -1
        self._selected_point = -1
        self._dragging = False
        self.update()

    def set_edit_spacing_nm(self, spacing_nm: float):
        self._edit_spacing_nm = max(0.0, float(spacing_nm))
        # Selection may no longer be an editable anchor after spacing changes.
        if self._selected_curve >= 0 and self._selected_point >= 0:
            editable = self._editable_indices(self._selected_curve)
            if self._selected_point not in editable:
                self._selected_curve = -1
                self._selected_point = -1
                self._dragging = False
        self.update()

    def _editable_indices(self, curve_index: int):
        if curve_index < 0 or curve_index >= len(self._curves):
            return []
        points = self._curves[curve_index][1]
        if not points:
            return []
        if self._edit_spacing_nm <= 0.0:
            return list(range(len(points)))

        spacing = self._edit_spacing_nm
        x0 = points[0][0]
        indices = []
        last_bucket = None
        for index, (x, _) in enumerate(points):
            bucket = int(round((x - x0) / spacing))
            target = x0 + bucket * spacing
            tolerance = max(1e-6, spacing * 0.12)
            if abs(x - target) <= tolerance and bucket != last_bucket:
                indices.append(index)
                last_bucket = bucket

        # Always expose both ends so interpolation has explicit boundaries.
        if 0 not in indices:
            indices.insert(0, 0)
        if len(points) - 1 not in indices:
            indices.append(len(points) - 1)
        return sorted(set(indices))

    def curve_points(self, curve_index: int):
        if curve_index < 0 or curve_index >= len(self._curves):
            return []
        return list(self._curves[curve_index][1])

    def _set_anchor_y(self, curve_index: int, point_index: int, value: float):
        name, points = self._curves[curve_index]
        x, _ = points[point_index]
        points[point_index] = (x, float(value))

        editable = self._editable_indices(curve_index)
        if point_index not in editable or len(editable) < 2:
            return

        anchor_pos = editable.index(point_index)
        spans = []
        if anchor_pos > 0:
            spans.append((editable[anchor_pos - 1], point_index))
        if anchor_pos + 1 < len(editable):
            spans.append((point_index, editable[anchor_pos + 1]))

        # Re-sample only the spans touching the edited anchor. The anchor X
        # coordinates remain measured/original; only Y values are interpolated.
        threshold = self._curve_break_threshold(points)
        for first, last in spans:
            # Never interpolate through a missing-data gap. A coarse edit
            # spacing controls measured samples within a continuous segment
            # only; it does not invent spectral values where the CSV is empty.
            if threshold is not None and any(
                points[index][0] - points[index - 1][0] > threshold
                for index in range(first + 1, last + 1)
            ):
                continue

            x0, y0 = points[first]
            x1, y1 = points[last]
            dx = x1 - x0
            if abs(dx) < 1e-12:
                continue
            for index in range(first + 1, last):
                xi, _ = points[index]
                t = (xi - x0) / dx
                points[index] = (xi, y0 + t * (y1 - y0))

    @staticmethod
    def _curve_break_threshold(points):
        deltas = [
            points[index][0] - points[index - 1][0]
            for index in range(1, len(points))
            if points[index][0] > points[index - 1][0]
        ]
        if not deltas:
            return None
        deltas = sorted(deltas)
        median = deltas[len(deltas) // 2]
        return median * 1.75

    def _draw_curve(self, painter, points, pen):
        if len(points) < 2:
            return
        threshold = self._curve_break_threshold(points)
        painter.setPen(pen)
        # drawPath() uses the painter's current brush as well as its pen.
        # Point handles set a brush below, so without resetting it here the
        # following open curve can be implicitly closed and filled. Curves
        # are always strokes only.
        painter.setBrush(Qt.BrushStyle.NoBrush)
        path = QPainterPath()
        path.moveTo(self._map_point(points[0][0], points[0][1]))
        previous_x = points[0][0]
        for x, y in points[1:]:
            mapped = self._map_point(x, y)
            if threshold is not None and x - previous_x > threshold:
                painter.drawPath(path)
                path = QPainterPath()
                path.moveTo(mapped)
            else:
                path.lineTo(mapped)
            previous_x = x
        painter.drawPath(path)

    def selected_point(self):
        if self._selected_curve < 0 or self._selected_point < 0:
            return None
        if self._selected_curve >= len(self._curves):
            return None
        name, points = self._curves[self._selected_curve]
        if self._selected_point >= len(points):
            return None
        x, y = points[self._selected_point]
        return self._selected_curve, self._selected_point, name, x, y

    def set_selected_y(self, value: float):
        selected = self.selected_point()
        if selected is None:
            return
        curve_index, point_index, name, x, old_y = selected
        value = float(value)
        if abs(value - old_y) < 1e-12:
            return
        self._set_anchor_y(curve_index, point_index, value)
        self.pointChanged.emit(curve_index, point_index, name, x, value)
        self.update()

    def _geometry(self):
        all_points = [
            point
            for _, points in self._curves
            for point in points
        ]
        original_points = [
            point
            for _, points in self._original_curves
            for point in points
        ]
        all_points += original_points
        if not all_points:
            return None

        x_min = min(point[0] for point in all_points)
        x_max = max(point[0] for point in all_points)
        y_min = min(point[1] for point in all_points)
        y_max = max(point[1] for point in all_points)

        if abs(x_max - x_min) < 1e-12:
            x_max = x_min + 1.0
        if abs(y_max - y_min) < 1e-12:
            y_max = y_min + 1.0

        y_padding = max(1e-6, 0.08 * (y_max - y_min))
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

        self._plot_rect = plot
        self._x_min = x_min
        self._x_max = x_max
        self._y_min = y_min
        self._y_max = y_max
        return plot, x_min, x_max, y_min, y_max

    def _map_point(self, x: float, y: float):
        plot = self._plot_rect
        px = plot.left() + (x - self._x_min) / (self._x_max - self._x_min) * plot.width()
        py = plot.bottom() - (y - self._y_min) / (self._y_max - self._y_min) * plot.height()
        return QPointF(px, py)

    def _value_from_y(self, py: float):
        plot = self._plot_rect
        t = (plot.bottom() - py) / max(1e-12, plot.height())
        return self._y_min + t * (self._y_max - self._y_min)

    def _hit_test(self, position, radius=9.0):
        if self._geometry() is None:
            return None
        best = None
        best_distance2 = radius * radius
        for curve_index, (name, points) in enumerate(self._curves):
            for point_index in self._editable_indices(curve_index):
                x, y = points[point_index]
                mapped = self._map_point(x, y)
                dx = mapped.x() - position.x()
                dy = mapped.y() - position.y()
                distance2 = dx * dx + dy * dy
                if distance2 <= best_distance2:
                    best_distance2 = distance2
                    best = (curve_index, point_index, name, x, y)
        return best

    def mousePressEvent(self, event):
        if event.button() != Qt.MouseButton.LeftButton:
            super().mousePressEvent(event)
            return

        hit = self._hit_test(event.position())
        if hit is None:
            self._selected_curve = -1
            self._selected_point = -1
            self._dragging = False
            self.update()
            event.accept()
            return

        curve_index, point_index, name, x, y = hit
        self._selected_curve = curve_index
        self._selected_point = point_index
        self._dragging = True
        self.pointSelected.emit(curve_index, point_index, name, x, y)
        self.update()
        event.accept()

    def mouseMoveEvent(self, event):
        if not self._dragging:
            super().mouseMoveEvent(event)
            return

        selected = self.selected_point()
        if selected is None or self._geometry() is None:
            return

        curve_index, point_index, name, x, _ = selected
        clamped_y = min(
            self._plot_rect.bottom(),
            max(self._plot_rect.top(), event.position().y()))
        y = self._value_from_y(clamped_y)
        self._set_anchor_y(curve_index, point_index, y)
        self.pointChanged.emit(curve_index, point_index, name, x, y)
        self.update()
        event.accept()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self._dragging:
            self._dragging = False
            event.accept()
            return
        super().mouseReleaseEvent(event)

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

        geometry = self._geometry()
        if geometry is None:
            painter.setPen(text_color)
            painter.drawText(
                self.rect(),
                Qt.AlignmentFlag.AlignCenter,
                "No curve data")
            return

        plot, x_min, x_max, y_min, y_max = geometry

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

        colors = [
            QColor("#e05252"),
            QColor("#59b66b"),
            QColor("#5d87d7"),
            QColor("#d6a84f"),
            QColor("#b56bd4"),
            QColor("#5bb9bf"),
        ]

        # Original CSV curves remain visible as a dim dashed reference.
        # Large wavelength gaps are intentionally not connected: those are
        # missing measurements, not a filled/interpolated section.
        for curve_index, (name, points) in enumerate(self._original_curves):
            color = QColor(colors[curve_index % len(colors)])
            color.setAlpha(80)
            pen = QPen(color, 1.0)
            pen.setStyle(Qt.PenStyle.DashLine)
            self._draw_curve(painter, points, pen)

        for curve_index, (name, points) in enumerate(self._curves):
            if len(points) < 2:
                continue

            color = colors[curve_index % len(colors)]
            self._draw_curve(painter, points, QPen(color, 1.8))

            editable = set(self._editable_indices(curve_index))
            for point_index, (x, y) in enumerate(points):
                if point_index not in editable:
                    continue
                point = self._map_point(x, y)
                selected = (
                    curve_index == self._selected_curve
                    and point_index == self._selected_point)
                painter.setPen(QPen(color, 1.4))
                painter.setBrush(
                    QColor(245, 210, 70)
                    if selected
                    else palette.color(self.backgroundRole()))
                radius = 5.5 if selected else 3.5
                painter.drawEllipse(point, radius, radius)

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

                x = self._map_point(stop_x, y_min).x()
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
                zero_px = self._map_point(zero_x, y_min).x()
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

        painter.setPen(text_color)
        painter.drawText(
            QRectF(4, plot.top() - 8, 56, 20),
            Qt.AlignmentFlag.AlignRight,
            f"{y_max:.4g}")
        painter.drawText(
            QRectF(4, plot.bottom() - 12, 56, 20),
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
            painter.setPen(QPen(colors[curve_index % len(colors)], 2.0))
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


class ImagePreviewWidget(QWidget):
    probeRequested = Signal(float, float)

    def __init__(self):
        super().__init__()
        self._image = QImage()
        self._reference_image = QImage()
        self._reference_wipe = 0.5
        self._message = "Convert an image to preview the result"
        self._probes = []
        self._color_space = APP_COLOR_SPACE

        self._fit_to_view = True
        self._zoom = 1.0
        self._pan = QPointF(0.0, 0.0)
        self._image_identity = None
        self._middle_panning = False
        self._last_pan_position = QPointF()

        self.setMinimumSize(520, 320)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_AcceptTouchEvents, True)
        self.setAcceptDrops(True)

        # Pixel probing is opt-in. The picker lives directly over the image
        # view so normal clicks remain available for navigation and do not
        # accidentally replace the current probe.
        self.picker_button = QPushButton(self)
        self.picker_button.setIcon(_probe_marker_icon(22))
        self.picker_button.setIconSize(QSize(22, 22))
        self.picker_button.setFixedSize(36, 36)
        self.picker_button.setCheckable(True)
        self.picker_button.setEnabled(False)
        self.picker_button.setToolTip(
            "Pick a color from the converted image and inspect that source "
            "pixel through the current FilmViz pipeline.")
        self.picker_button.toggled.connect(self._picker_toggled)
        self.picker_button.raise_()

        # Agent snapshot copy lives beside the color picker. A normal click
        # copies the complete diagnostic board; right-click exposes focused
        # copy variants without adding more permanent controls to the view.
        self.copy_button = QPushButton(self)
        self.copy_button.setIcon(_copy_icon(22))
        self.copy_button.setIconSize(QSize(22, 22))
        self.copy_button.setFixedSize(36, 36)
        self.copy_button.setEnabled(False)
        self.copy_button.setToolTip(
            "Copy an agent snapshot: converted image, FilmViz settings, "
            "probe data, and all scopes.")
        self.copy_button.clicked.connect(self._copy_agent_snapshot)
        self.copy_button.setContextMenuPolicy(
            Qt.ContextMenuPolicy.CustomContextMenu)
        self.copy_button.customContextMenuRequested.connect(
            self._show_copy_menu)
        self.copy_button.raise_()

        self.clear_probe_button = QPushButton(self)
        self.clear_probe_button.setIcon(_trash_icon(22))
        self.clear_probe_button.setIconSize(QSize(22, 22))
        self.clear_probe_button.setFixedSize(36, 36)
        self.clear_probe_button.setEnabled(False)
        self.clear_probe_button.setToolTip("Clear all picked color markers")
        self.clear_probe_button.clicked.connect(self._clear_probes)
        self.clear_probe_button.raise_()

    def set_rgb(
        self,
        width: int,
        height: int,
        rgb: bytes,
        image_identity=None,
    ):
        preserve_view = (
            image_identity is not None
            and image_identity == self._image_identity
            and not self._image.isNull()
            and self._image.width() == width
            and self._image.height() == height
        )

        fit_to_view = self._fit_to_view
        zoom = self._zoom
        pan = QPointF(self._pan)

        image = QImage(
            rgb,
            width,
            height,
            width * 3,
            QImage.Format.Format_RGB888)
        image.setColorSpace(self._color_space)
        self._image = image.copy()
        self._image_identity = image_identity
        self._message = ""
        self.picker_button.setEnabled(True)
        self.copy_button.setEnabled(True)

        if preserve_view:
            self._fit_to_view = fit_to_view
            self._zoom = zoom
            self._pan = pan
            self.update()
        else:
            self.fit_to_view()

    def has_image(self):
        return not self._image.isNull()

    def has_reference(self):
        return not self._reference_image.isNull()

    def set_reference_rgb(self, width: int, height: int, rgb: bytes):
        image = QImage(
            rgb,
            width,
            height,
            width * 3,
            QImage.Format.Format_RGB888)
        image.setColorSpace(self._color_space)
        self._reference_image = image.copy()
        self.update()

    def clear_reference(self):
        self._reference_image = QImage()
        self.update()

    def set_reference_wipe(self, value: int):
        self._reference_wipe = min(1.0, max(0.0, float(value) / 100.0))
        self.update()

    def set_color_space(self, color_space):
        self._color_space = color_space
        if not self._image.isNull():
            self._image.setColorSpace(color_space)
        if not self._reference_image.isNull():
            self._reference_image.setColorSpace(color_space)
        self.update()

    def set_error(self, message: str):
        self._image = QImage()
        self._reference_image = QImage()
        self._message = message
        self._probes = []
        self._image_identity = None
        self._fit_to_view = True
        self._zoom = 1.0
        self._pan = QPointF(0.0, 0.0)
        self.picker_button.setChecked(False)
        self.picker_button.setEnabled(False)
        self.copy_button.setEnabled(False)
        self.clear_probe_button.setEnabled(False)
        self.update()

    def fit_to_view(self):
        self._fit_to_view = True
        self._zoom = 1.0
        self._pan = QPointF(0.0, 0.0)
        self.update()

    def _fit_scale(self):
        if self._image.isNull():
            return 1.0

        return min(
            self.width() / max(1, self._image.width()),
            self.height() / max(1, self._image.height()))

    def _effective_scale(self):
        fit_scale = self._fit_scale()
        if self._fit_to_view:
            return fit_scale
        return fit_scale * self._zoom

    def _image_rect(self):
        if self._image.isNull():
            return QRectF()

        scale = self._effective_scale()
        width = self._image.width() * scale
        height = self._image.height() * scale

        center = QPointF(
            self.width() * 0.5,
            self.height() * 0.5) + self._pan

        return QRectF(
            center.x() - width * 0.5,
            center.y() - height * 0.5,
            width,
            height)

    def _image_uv_from_widget(self, point):
        rect = self._image_rect()
        if rect.isEmpty() or not rect.contains(point):
            return None

        u = (point.x() - rect.left()) / max(1e-12, rect.width())
        v = (point.y() - rect.top()) / max(1e-12, rect.height())

        return (
            min(1.0, max(0.0, float(u))),
            min(1.0, max(0.0, float(v))),
        )

    def _zoom_at(self, widget_position, factor: float):
        if self._image.isNull():
            return

        factor = max(0.1, min(10.0, float(factor)))
        old_rect = self._image_rect()

        if old_rect.isEmpty():
            return

        # Preserve the image point under the gesture/cursor while zooming.
        image_x = (
            (widget_position.x() - old_rect.left())
            / max(1e-12, old_rect.width()))
        image_y = (
            (widget_position.y() - old_rect.top())
            / max(1e-12, old_rect.height()))

        if self._fit_to_view:
            self._fit_to_view = False
            self._zoom = 1.0

        self._zoom = max(
            0.05,
            min(32.0, self._zoom * factor))

        new_scale = self._effective_scale()
        new_width = self._image.width() * new_scale
        new_height = self._image.height() * new_scale

        desired_left = widget_position.x() - image_x * new_width
        desired_top = widget_position.y() - image_y * new_height

        new_center = QPointF(
            desired_left + new_width * 0.5,
            desired_top + new_height * 0.5)

        widget_center = QPointF(
            self.width() * 0.5,
            self.height() * 0.5)

        self._pan = new_center - widget_center
        self.update()

    @Slot(bool)
    def _picker_toggled(self, enabled: bool):
        if enabled and not self._image.isNull():
            self.setCursor(Qt.CursorShape.CrossCursor)
        else:
            self.unsetCursor()

    def _position_picker_button(self):
        margin = 8
        spacing = 6
        y = max(
            margin,
            self.height() - self.picker_button.height() - margin)

        self.picker_button.move(margin, y)
        self.clear_probe_button.move(
            margin + self.picker_button.width() + spacing,
            y)
        self.copy_button.move(
            margin + self.picker_button.width() + spacing
            + self.clear_probe_button.width() + spacing,
            y)
        self.picker_button.raise_()
        self.copy_button.raise_()
        self.clear_probe_button.raise_()

    def set_probes(self, points):
        self._probes = [(float(u), float(v)) for u, v in points]
        self.clear_probe_button.setEnabled(bool(self._probes))
        self.update()

    @Slot()
    def _clear_probes(self):
        window = self.window()
        clearer = getattr(window, "_clear_probes", None)
        if clearer is not None:
            clearer()

    @Slot()
    def _copy_agent_snapshot(self):
        window = self.window()
        copier = getattr(window, "_copy_agent_snapshot", None)
        if copier is not None:
            copier()

    @Slot(QPoint)
    def _show_copy_menu(self, position):
        window = self.window()
        if window is None:
            return

        menu = QMenu(self)
        snapshot_action = menu.addAction("Copy agent snapshot")
        settings_action = menu.addAction("Copy settings and probe text")
        image_action = menu.addAction("Copy converted image")
        marked_image_action = menu.addAction("Copy converted image with markers")

        action = menu.exec(self.copy_button.mapToGlobal(position))
        if action == snapshot_action:
            copier = getattr(window, "_copy_agent_snapshot", None)
            if copier is not None:
                copier()
        elif action == settings_action:
            copier = getattr(window, "_copy_agent_text", None)
            if copier is not None:
                copier()
        elif action == image_action:
            copier = getattr(window, "_copy_converted_image", None)
            if copier is not None:
                copier()
        elif action == marked_image_action:
            copier = getattr(window, "_copy_converted_image_with_markers", None)
            if copier is not None:
                copier()

    def converted_image_with_markers(self):
        if self._image.isNull():
            return QImage()

        image = self._image.copy()
        if not self._probes:
            return image

        painter = QPainter(image)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        # Keep markers clearly visible when the native converted image is
        # copied, independent of preview zoom. Scale their size gently from
        # the image dimensions rather than using the preview's screen pixels.
        minimum_dimension = max(1, min(image.width(), image.height()))
        radius = max(6.0, minimum_dimension * 0.004)
        cross = radius * 1.7
        pen_width = max(1.0, minimum_dimension * 0.00065)

        font = painter.font()
        font.setPixelSize(max(12, int(round(radius * 1.8))))
        painter.setFont(font)
        painter.setPen(QPen(QColor(245, 210, 70), pen_width))

        for index, (u, v) in enumerate(self._probes, start=1):
            x = float(u) * image.width()
            y = float(v) * image.height()
            point = QPointF(x, y)

            painter.drawEllipse(point, radius, radius)
            painter.drawLine(QPointF(x - cross, y), QPointF(x + cross, y))
            painter.drawLine(QPointF(x, y - cross), QPointF(x, y + cross))
            painter.drawText(
                QRectF(
                    x + radius * 1.35,
                    y - radius * 1.65,
                    radius * 5.0,
                    radius * 2.5),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                str(index))

        painter.end()
        image.setColorSpace(self._color_space)
        return image

    def rgb_at(self, u: float, v: float):
        if self._image.isNull():
            return None

        x = min(
            self._image.width() - 1,
            max(0, int(u * self._image.width())))
        y = min(
            self._image.height() - 1,
            max(0, int(v * self._image.height())))
        color = self._image.pixelColor(x, y)
        return (color.redF(), color.greenF(), color.blueF())

    def dragEnterEvent(self, event):
        if self._image.isNull():
            event.ignore()
            return

        urls = event.mimeData().urls()
        if len(urls) != 1 or not urls[0].isLocalFile():
            event.ignore()
            return

        suffix = Path(urls[0].toLocalFile()).suffix.lower()
        if suffix not in (".exr", ".dpx", ".tif", ".tiff", ".png", ".jpg", ".jpeg"):
            event.ignore()
            return

        event.acceptProposedAction()

    def dropEvent(self, event):
        if self._image.isNull():
            event.ignore()
            return

        urls = event.mimeData().urls()
        if len(urls) != 1 or not urls[0].isLocalFile():
            event.ignore()
            return

        window = self.window()
        loader = getattr(window, "_load_reference_image", None)
        if loader is None:
            event.ignore()
            return

        loader(urls[0].toLocalFile())
        event.acceptProposedAction()

    def mousePressEvent(self, event):
        if self._image.isNull():
            return

        # A physical middle mouse button is always navigation, even while the
        # color picker is armed. Dragging pans the converted image and the
        # reference overlay together.
        if event.button() == Qt.MouseButton.MiddleButton:
            self.setFocus(Qt.FocusReason.MouseFocusReason)
            self._fit_to_view = False
            self._middle_panning = True
            self._last_pan_position = event.position()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            event.accept()
            return

        # Right-click cancels color picking without creating or removing any
        # existing sample points. Leave right-click untouched when the picker
        # is not active so it remains available for future context actions.
        if (event.button() == Qt.MouseButton.RightButton
                and self.picker_button.isChecked()):
            self.picker_button.setChecked(False)
            event.accept()
            return

        # Pixel probing only happens after the picker button has been armed.
        # Normal clicks therefore never create probe/log output by accident.
        if not self.picker_button.isChecked():
            event.ignore()
            return

        # Restrict the actual pick to a real left mouse click. Ignore mouse
        # events synthesized by macOS from trackpad taps/navigation gestures.
        if event.button() != Qt.MouseButton.LeftButton:
            event.ignore()
            return

        if event.source() != Qt.MouseEventSource.MouseEventNotSynthesized:
            event.ignore()
            return

        self.setFocus(Qt.FocusReason.MouseFocusReason)

        uv = self._image_uv_from_widget(event.position())
        if uv is None:
            event.ignore()
            return

        u, v = uv
        self.probeRequested.emit(float(u), float(v))
        event.accept()

    def mouseMoveEvent(self, event):
        if self._middle_panning:
            delta = event.position() - self._last_pan_position
            self._last_pan_position = event.position()
            self._pan += delta
            self.update()
            event.accept()
            return

        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        if (
            event.button() == Qt.MouseButton.MiddleButton
            and self._middle_panning
        ):
            self._middle_panning = False
            if self.picker_button.isChecked():
                self.setCursor(Qt.CursorShape.CrossCursor)
            else:
                self.unsetCursor()
            event.accept()
            return

        super().mouseReleaseEvent(event)

    def wheelEvent(self, event):
        if self._image.isNull():
            event.ignore()
            return

        pixel_delta = event.pixelDelta()
        angle_delta = event.angleDelta()
        modifiers = event.modifiers()

        zoom_modifier = (
            modifiers & Qt.KeyboardModifier.ControlModifier
            or modifiers & Qt.KeyboardModifier.MetaModifier
        )

        # Ctrl/Command + trackpad/wheel always zooms, including on macOS
        # where a trackpad commonly provides pixelDelta rather than
        # angleDelta. Pinch-to-zoom is handled by NativeGesture below.
        if zoom_modifier:
            if not pixel_delta.isNull():
                delta = pixel_delta.y()
                if delta != 0:
                    factor = 1.01 ** delta
                    self._zoom_at(event.position(), factor)
                    event.accept()
                    return

            if not angle_delta.isNull():
                factor = 1.15 ** (angle_delta.y() / 120.0)
                self._zoom_at(event.position(), factor)
                event.accept()
                return

        # Plain two-finger trackpad scrolling pans the converted image and
        # reference together. Qt normally reports a trackpad through
        # pixelDelta(), while a physical mouse wheel reports angleDelta().
        if not pixel_delta.isNull():
            self._fit_to_view = False
            self._pan += QPointF(
                pixel_delta.x(),
                pixel_delta.y())
            self.update()
            event.accept()
            return

        # A physical mouse wheel zooms without requiring a modifier. Keep the
        # image point under the cursor stable, matching pinch zoom behavior.
        if not angle_delta.isNull():
            factor = 1.15 ** (angle_delta.y() / 120.0)
            self._zoom_at(event.position(), factor)
            event.accept()
            return

        event.ignore()


    def event(self, event):
        if event.type() == QEvent.Type.NativeGesture:
            gesture = event.gestureType()

            if gesture == Qt.NativeGestureType.ZoomNativeGesture:
                # macOS pinch values are incremental. Positive values zoom in,
                # negative values zoom out. Keep the point under the gesture
                # stable while changing scale.
                factor = max(0.25, 1.0 + float(event.value()))
                try:
                    position = event.position()
                except AttributeError:
                    position = QPointF(
                        self.width() * 0.5,
                        self.height() * 0.5)

                self._zoom_at(position, factor)
                event.accept()
                return True

            if gesture == Qt.NativeGestureType.PanNativeGesture:
                try:
                    delta = event.delta()
                except AttributeError:
                    delta = QPointF(0.0, 0.0)

                self._fit_to_view = False
                self._pan += QPointF(
                    delta.x(),
                    delta.y())
                self.update()
                event.accept()
                return True

        return super().event(event)


    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_F:
            self.fit_to_view()
            event.accept()
            return

        super().keyPressEvent(event)

    def resizeEvent(self, event):
        # Fit mode should continuously follow the available widget size.
        if self._fit_to_view:
            self.update()

        self._position_picker_button()
        super().resizeEvent(event)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#111315"))

        if self._image.isNull():
            painter.setPen(self.palette().color(self.foregroundRole()))
            painter.drawText(
                self.rect().adjusted(20, 20, -20, -20),
                Qt.AlignmentFlag.AlignCenter | Qt.TextFlag.TextWordWrap,
                self._message)
            return

        pixmap = QPixmap.fromImage(self._image)
        rect = self._image_rect()

        painter.setRenderHint(
            QPainter.RenderHint.SmoothPixmapTransform,
            self._effective_scale() < 1.0)

        painter.drawPixmap(
            rect.toRect(),
            pixmap)

        if not self._reference_image.isNull():
            reference_pixmap = QPixmap.fromImage(self._reference_image)
            wipe_x = rect.left() + self._reference_wipe * rect.width()
            clip_rect = QRectF(
                rect.left(),
                rect.top(),
                max(0.0, wipe_x - rect.left()),
                rect.height())

            painter.save()
            painter.setClipRect(clip_rect)
            painter.drawPixmap(
                rect.toRect(),
                reference_pixmap)
            painter.restore()

            painter.setPen(QPen(QColor(245, 210, 70), 1.0))
            painter.drawLine(
                QPointF(wipe_x, rect.top()),
                QPointF(wipe_x, rect.bottom()))

        for index, (u, v) in enumerate(self._probes, start=1):
            x = rect.left() + u * rect.width()
            y = rect.top() + v * rect.height()

            painter.setPen(QPen(QColor(245, 210, 70), 1.0))
            painter.drawEllipse(QPointF(x, y), 6.0, 6.0)
            painter.drawLine(QPointF(x - 10, y), QPointF(x + 10, y))
            painter.drawLine(QPointF(x, y - 10), QPointF(x, y + 10))
            painter.drawText(
                QRectF(x + 8, y - 14, 28, 18),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                str(index))


class VectorScopeWidget(QWidget):
    def __init__(self):
        super().__init__()
        self._samples = []
        self._probe_rgbs = []
        self._zoom2 = False
        self.setMinimumSize(300, 250)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)

    @staticmethod
    def _ycbcr(r, g, b):
        # Full-range BT.709 Y'CbCr. The preview pixels are already encoded
        # Rec.709/Gamma 2.4, so scopes intentionally operate on display-domain
        # R'G'B' just like a conventional video vectorscope.
        y = 0.2126 * r + 0.7152 * g + 0.0722 * b
        cb = (b - y) / 1.8556
        cr = (r - y) / 1.5748
        return y, cb, cr

    def set_rgb(self, width: int, height: int, rgb):
        pixel_count = width * height
        stride = max(1, pixel_count // 70000)
        samples = []

        for pixel in range(0, pixel_count, stride):
            offset = pixel * 3
            r = float(rgb[offset])
            g = float(rgb[offset + 1])
            b = float(rgb[offset + 2])

            _, cb, cr = self._ycbcr(r, g, b)
            samples.append((cb, cr, r, g, b))

        self._samples = samples
        self.update()

    def set_probe(self, u: float, v: float, rgb):
        self._probe_rgbs.append(tuple(rgb))
        self.update()

    def clear_probe(self):
        self._probe_rgbs = []
        self.update()

    def set_zoom2(self, enabled: bool):
        self._zoom2 = bool(enabled)
        self.update()

    def _scope_point(self, center, radius, cb, cr, zoom=1.0):
        # A full-range BT.709 primary reaches approximately +/-0.5 chroma.
        # Scale 0.5 to the outer reference circle.
        scale = 2.0 * zoom
        return QPointF(
            center.x() + cb * radius * scale,
            center.y() - cr * radius * scale)

    def _target(self, rgb):
        r, g, b = rgb
        _, cb, cr = self._ycbcr(r, g, b)
        return cb, cr

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.fillRect(self.rect(), QColor("#050607"))

        margin = 24.0
        side = max(1.0, min(self.width(), self.height()) - 2.0 * margin)
        left = (self.width() - side) * 0.5
        top = (self.height() - side) * 0.5
        center = QPointF(left + side * 0.5, top + side * 0.5)
        radius = side * 0.47

        gold = QColor(145, 116, 10)
        gold_text = QColor(185, 154, 36)
        grid = QColor(80, 86, 88, 150)

        # Outer circle / axes.
        painter.setPen(QPen(gold, 1.15))
        painter.drawEllipse(center, radius, radius)

        painter.setPen(QPen(grid, 1.0))
        painter.drawLine(
            QPointF(center.x() - radius, center.y()),
            QPointF(center.x() + radius, center.y()))
        painter.drawLine(
            QPointF(center.x(), center.y() - radius),
            QPointF(center.x(), center.y() + radius))

        # Resolve-like radial tick ring: long every 30 deg, medium every 10,
        # short every 5.
        import math
        for degrees in range(0, 360, 5):
            angle = math.radians(degrees)
            if degrees % 30 == 0:
                length = radius * 0.080
                width = 1.3
            elif degrees % 10 == 0:
                length = radius * 0.050
                width = 1.0
            else:
                length = radius * 0.028
                width = 0.8

            x0 = center.x() + math.cos(angle) * radius
            y0 = center.y() + math.sin(angle) * radius
            x1 = center.x() + math.cos(angle) * (radius - length)
            y1 = center.y() + math.sin(angle) * (radius - length)
            painter.setPen(QPen(gold, width))
            painter.drawLine(QPointF(x0, y0), QPointF(x1, y1))

        # Skin-tone indicator. Conventional flesh line is approximately
        # 123 degrees in the Cb/Cr chroma plane (between Y and R).
        skin_angle = math.radians(123.0)
        skin_cb = math.cos(skin_angle) * 0.5
        skin_cr = math.sin(skin_angle) * 0.5
        skin_end = self._scope_point(
            center,
            radius,
            skin_cb,
            skin_cr,
            1.0)
        skin_pen = QPen(QColor(125, 125, 125, 170), 1.3)
        painter.setPen(skin_pen)
        painter.drawLine(center, skin_end)

        # SMPTE/BT.709 75% color-bar target locations. This is the important
        # part that was lost in the simplified scope: boxes and labels are now
        # derived from the same Y'CbCr transform as the samples.
        targets = (
            ("R", (0.75, 0.00, 0.00)),
            ("M", (0.75, 0.00, 0.75)),
            ("B", (0.00, 0.00, 0.75)),
            ("C", (0.00, 0.75, 0.75)),
            ("G", (0.00, 0.75, 0.00)),
            ("Y", (0.75, 0.75, 0.00)),
        )

        painter.setPen(QPen(gold_text, 1.2))
        for label, rgb_value in targets:
            cb, cr = self._target(rgb_value)
            point = self._scope_point(
                center,
                radius,
                cb,
                cr,
                1.0)

            box_size = max(10.0, radius * 0.075)
            painter.drawRect(
                QRectF(
                    point.x() - box_size * 0.5,
                    point.y() - box_size * 0.5,
                    box_size,
                    box_size))

            # Put labels radially just outside the target boxes.
            dx = point.x() - center.x()
            dy = point.y() - center.y()
            distance = max(1.0, math.hypot(dx, dy))
            lx = point.x() + dx / distance * 18.0
            ly = point.y() + dy / distance * 18.0
            painter.drawText(
                QRectF(lx - 12, ly - 10, 24, 20),
                Qt.AlignmentFlag.AlignCenter,
                label)

        # Bright center reference.
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(230, 230, 230, 220))
        painter.drawEllipse(center, 2.0, 2.0)

        zoom = 2.0 if self._zoom2 else 1.0

        # Use small translucent points with a second faint halo pass. Dense
        # areas naturally build up into the soft luminous traces seen in
        # Resolve without smearing the actual chroma positions.
        painter.setPen(Qt.PenStyle.NoPen)
        for cb, cr, r, g, b in self._samples:
            point = self._scope_point(
                center,
                radius,
                cb,
                cr,
                zoom)

            # Do not clip chroma to the nominal reference circle. Real
            # floating-point output can legitimately exceed the 0..1 display
            # gamut and should remain visible outside the vectorscope ring.
            color = QColor.fromRgbF(
                max(0.0, min(1.0, r)),
                max(0.0, min(1.0, g)),
                max(0.0, min(1.0, b)),
                0.055)
            painter.setBrush(color)
            painter.drawEllipse(point, 1.5, 1.5)

            halo = QColor.fromRgbF(
                max(0.0, min(1.0, r)),
                max(0.0, min(1.0, g)),
                max(0.0, min(1.0, b)),
                0.018)
            painter.setBrush(halo)
            painter.drawEllipse(point, 2.8, 2.8)

        for index, probe_rgb in enumerate(self._probe_rgbs, start=1):
            _, cb, cr = self._ycbcr(*probe_rgb)
            point = self._scope_point(
                center,
                radius,
                cb,
                cr,
                zoom)
            probe_pen = QPen(QColor(245, 210, 70), 1.8)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(probe_pen)
            painter.drawEllipse(point, 7.0, 7.0)
            painter.drawLine(
                QPointF(point.x() - 10.0, point.y()),
                QPointF(point.x() + 10.0, point.y()))
            painter.drawLine(
                QPointF(point.x(), point.y() - 10.0),
                QPointF(point.x(), point.y() + 10.0))
            painter.drawText(
                QRectF(point.x() + 8, point.y() - 13, 24, 18),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                str(index))


class HistogramWidget(QWidget):
    def __init__(self):
        super().__init__()
        self._histograms = None
        self._probe_rgbs = []
        self._range_min = 0.0
        self._range_max = 1.0
        self._bins = 512
        self.setMinimumSize(260, 220)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)

    def set_rgb(self, width: int, height: int, rgb):
        # Analyze the complete floating-point output. Use NumPy when available
        # so the result is identical to the scalar implementation without
        # spending Python time on every pixel.
        pixel_count = width * height

        if np is not None:
            values = np.asarray(rgb, dtype=np.float32).reshape(-1, 3)

            minimum = min(0.0, float(np.min(values)))
            maximum = max(1.0, float(np.max(values)))

            padding = max(0.01, (maximum - minimum) * 0.01)
            self._range_min = minimum - padding if minimum < 0.0 else 0.0
            self._range_max = maximum + padding if maximum > 1.0 else 1.0

            histograms = []
            histogram_range = (self._range_min, self._range_max)
            for channel in range(3):
                counts, _ = np.histogram(
                    values[:, channel],
                    bins=self._bins,
                    range=histogram_range)
                histograms.append(counts.tolist())

            self._histograms = histograms
            self.update()
            return

        minimum = 0.0
        maximum = 1.0
        for pixel in range(pixel_count):
            offset = pixel * 3
            minimum = min(
                minimum,
                float(rgb[offset]),
                float(rgb[offset + 1]),
                float(rgb[offset + 2]))
            maximum = max(
                maximum,
                float(rgb[offset]),
                float(rgb[offset + 1]),
                float(rgb[offset + 2]))

        padding = max(0.01, (maximum - minimum) * 0.01)
        self._range_min = minimum - padding if minimum < 0.0 else 0.0
        self._range_max = maximum + padding if maximum > 1.0 else 1.0

        histograms = [[0] * self._bins for _ in range(3)]
        span = max(1e-12, self._range_max - self._range_min)

        for pixel in range(pixel_count):
            offset = pixel * 3
            for channel in range(3):
                value = float(rgb[offset + channel])
                normalized = (value - self._range_min) / span
                index = min(
                    self._bins - 1,
                    max(0, int(normalized * self._bins)))
                histograms[channel][index] += 1

        self._histograms = histograms
        self.update()

    def set_probe(self, u: float, v: float, rgb):
        self._probe_rgbs.append(tuple(rgb))
        self.update()

    def clear_probe(self):
        self._probe_rgbs = []
        self.update()

    def paintEvent(self, event):
        import math

        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.fillRect(self.rect(), QColor("#0b0d0f"))

        left_margin = 42
        right_margin = 12
        top_margin = 12
        label_height = 18
        bottom_margin = 12 + label_height

        plot = self.rect().adjusted(
            left_margin,
            top_margin,
            -right_margin,
            -bottom_margin)

        grid_color = QColor(75, 75, 75)
        minor_grid_color = QColor(42, 42, 42)
        label_color = QColor(185, 154, 36)

        painter.setPen(QPen(grid_color, 1.0))
        painter.drawRect(plot)

        span = max(1e-12, self._range_max - self._range_min)

        # Reference lines are always anchored to the normal encoded 0..1
        # range, even when the float histogram expands beyond it.
        for percent in range(0, 101, 20):
            value = percent / 100.0
            if value < self._range_min or value > self._range_max:
                continue
            x = (
                plot.left()
                + (value - self._range_min) / span
                * plot.width())
            painter.setPen(
                QPen(
                    grid_color if percent in (0, 100)
                    else minor_grid_color,
                    1.0))
            painter.drawLine(
                QPointF(x, plot.top()),
                QPointF(x, plot.bottom()))
            painter.setPen(label_color)
            painter.drawText(
                QRectF(x - 18, plot.bottom() + 2, 36, label_height),
                Qt.AlignmentFlag.AlignCenter,
                str(percent))

        if self._histograms is None:
            painter.setPen(self.palette().color(self.foregroundRole()))
            painter.drawText(
                plot,
                Qt.AlignmentFlag.AlignCenter,
                "RGB histogram")
            return

        # Logarithmic population scaling prevents a dominant black/background
        # bin from flattening all useful midtone and highlight information.
        log_maximum = max(
            math.log1p(max(values))
            for values in self._histograms)
        if log_maximum <= 0.0:
            return

        colors = (
            QColor("#ef5555"),
            QColor("#55c477"),
            QColor("#5b8def"),
        )

        for channel, values in enumerate(self._histograms):
            path = QPainterPath()
            for index, count in enumerate(values):
                x = (
                    plot.left()
                    + index / max(1, len(values) - 1)
                    * plot.width())
                y = (
                    plot.bottom()
                    - math.log1p(count) / log_maximum
                    * plot.height())
                if index == 0:
                    path.moveTo(x, y)
                else:
                    path.lineTo(x, y)
            painter.setPen(QPen(colors[channel], 1.3))
            painter.drawPath(path)

        painter.setPen(QPen(QColor(245, 210, 70), 1.3))
        for probe_rgb in self._probe_rgbs:
            for value in probe_rgb:
                value = float(value)
                if value < self._range_min or value > self._range_max:
                    continue
                x = (
                    plot.left()
                    + (value - self._range_min) / span
                    * plot.width())
                painter.drawLine(
                    QPointF(x, plot.top()),
                    QPointF(x, plot.bottom()))

        painter.setPen(self.palette().color(self.foregroundRole()))
        range_text = "RGB histogram — log population"
        if self._range_min < 0.0 or self._range_max > 1.0:
            range_text += (
                f"  [{self._range_min:.3g}, "
                f"{self._range_max:.3g}]")
        painter.drawText(
            QRectF(
                plot.left(),
                plot.bottom() + 2,
                plot.width(),
                label_height),
            Qt.AlignmentFlag.AlignCenter,
            range_text)


class ParadeWidget(QWidget):
    def __init__(self):
        super().__init__()
        self._samples = None
        self._probes = []
        self.setMinimumSize(240, 200)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)

    def set_rgb(self, width: int, height: int, rgb):
        # Build the parade directly from floating-point output pixels.
        columns = 256
        bins = 512
        pixel_count = width * height
        stride = max(1, pixel_count // 180000)

        if np is not None:
            values = np.asarray(rgb, dtype=np.float32).reshape(-1, 3)
            indices = np.arange(0, pixel_count, stride, dtype=np.int64)
            sampled = values[indices]

            x = indices % width

            # Keep the actual sampled pixels for display. The binned arrays
            # remain useful for probe/analysis compatibility, but the visible
            # waveform is rendered from these raw samples so it behaves like
            # a true low-opacity intensity cloud rather than a block histogram.
            x_normalized = (
                x.astype(np.float32)
                / max(1.0, float(width - 1))
            )
            valid = np.all(np.isfinite(sampled), axis=1)
            valid &= np.any(
                (sampled >= 0.0) & (sampled <= 1.0),
                axis=1)
            cloud = np.column_stack((
                x_normalized[valid],
                sampled[valid, 0],
                sampled[valid, 1],
                sampled[valid, 2],
            ))
            self._waveform_points = cloud.tolist()
            column_indices = np.minimum(
                columns - 1,
                (x * columns // max(1, width)).astype(np.int64))

            bin_indices = np.clip(
                (sampled * (bins - 1)).astype(np.int64),
                0,
                bins - 1)

            parade = np.zeros(
                (3, columns, bins),
                dtype=np.int32)

            for channel in range(3):
                np.add.at(
                    parade[channel],
                    (column_indices, bin_indices[:, channel]),
                    1)

            self._samples = parade
            self.update()
            return

        parade = [
            [[0] * bins for _ in range(columns)]
            for _ in range(3)
        ]

        for pixel in range(0, pixel_count, stride):
            x = pixel % width
            column = min(
                columns - 1,
                int(x * columns / max(1, width)))
            offset = pixel * 3

            for channel in range(3):
                value = float(rgb[offset + channel])
                bin_index = min(
                    bins - 1,
                    max(0, int(value * (bins - 1))))
                parade[channel][column][bin_index] += 1

        self._samples = parade
        self.update()

    def set_probe(self, u: float, v: float, rgb):
        self._probes.append((float(u), tuple(rgb)))
        self.update()

    def clear_probe(self):
        self._probes = []
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)
        painter.fillRect(self.rect(), QColor("#0b0d0f"))

        left_margin = 42
        right_margin = 12
        top_margin = 12
        label_height = 18
        bottom_margin = 12 + label_height
        gap = 8

        plot = self.rect().adjusted(
            left_margin,
            top_margin,
            -right_margin,
            -bottom_margin)

        grid_color = QColor(75, 75, 75)
        minor_grid_color = QColor(42, 42, 42)
        label_color = QColor(185, 154, 36)

        painter.setPen(QPen(grid_color, 1.0))
        painter.drawRect(plot)

        for percent in range(0, 101, 10):
            y = (
                plot.bottom()
                - (percent / 100.0)
                * plot.height())

            painter.setPen(
                QPen(
                    grid_color if percent % 20 == 0
                    else minor_grid_color,
                    1.0))

            painter.drawLine(
                QPointF(plot.left(), y),
                QPointF(plot.right(), y))

            painter.setPen(label_color)
            painter.drawText(
                QRectF(
                    2,
                    y - 8,
                    left_margin - 7,
                    16),
                Qt.AlignmentFlag.AlignRight
                | Qt.AlignmentFlag.AlignVCenter,
                str(percent))

        third = (plot.width() - 2 * gap) / 3.0
        channel_rects = [
            QRectF(
                plot.left() + channel * (third + gap),
                plot.top(),
                third,
                plot.height())
            for channel in range(3)
        ]

        for rect in channel_rects[1:]:
            painter.setPen(QPen(QColor(55, 55, 55), 1.0))
            painter.drawLine(
                QPointF(rect.left() - gap * 0.5, plot.top()),
                QPointF(rect.left() - gap * 0.5, plot.bottom()))

        if self._samples is None:
            painter.setPen(self.palette().color(self.foregroundRole()))
            painter.drawText(
                plot,
                Qt.AlignmentFlag.AlignCenter,
                "RGB parade")
            return

        colors = (
            QColor("#ef5555"),
            QColor("#55c477"),
            QColor("#5b8def"),
        )

        for channel, rect in enumerate(channel_rects):
            data = self._samples[channel]
            maximum = max(
                int(max(column))
                for column in data)

            if maximum <= 0:
                continue

            painter.setPen(Qt.PenStyle.NoPen)

            for x_index, column in enumerate(data):
                px = (
                    rect.left()
                    + x_index / 255.0 * rect.width())

                for value, count in enumerate(column):
                    if count <= 0:
                        continue

                    # Log-style intensity keeps sparse and dense areas visible.
                    alpha = min(
                        0.75,
                        0.08
                        + 0.67
                        * (
                            (count / maximum) ** 0.35))

                    color = QColor(colors[channel])
                    color.setAlphaF(alpha)
                    painter.setBrush(color)

                    py = (
                        rect.bottom()
                        - value / max(1, len(column) - 1)
                        * rect.height())

                    painter.drawRect(
                        QRectF(px, py, 1.2, 1.2))

        probe_pen = QPen(QColor(245, 210, 70), 1.8)
        painter.setBrush(QColor(245, 210, 70))
        painter.setPen(probe_pen)
        for u, rgb in self._probes:
            for channel, rect in enumerate(channel_rects):
                px = rect.left() + u * rect.width()
                py = rect.bottom() - rgb[channel] * rect.height()
                painter.drawEllipse(QPointF(px, py), 4.5, 4.5)

        painter.setPen(self.palette().color(self.foregroundRole()))
        painter.drawText(
            QRectF(
                plot.left(),
                plot.bottom() + 2,
                plot.width(),
                label_height),
            Qt.AlignmentFlag.AlignCenter,
            "RGB parade")


class WaveformWidget(QWidget):
    def __init__(self):
        super().__init__()
        self._waveform = None
        self._luma_waveform = None
        self._waveform_points = []
        self._probes = []
        self._mode = "rgb"
        self.setMinimumSize(240, 200)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)

    def set_rgb(self, width: int, height: int, rgb):
        # Fast diagnostic waveform: accumulate into a compact fixed grid.
        # Rendering cost depends on occupied bins, not on source pixel count.
        columns = 320
        bins = 256
        pixel_count = width * height
        stride = max(1, pixel_count // 180000)

        if np is not None:
            values = np.asarray(rgb, dtype=np.float32).reshape(-1, 3)
            indices = np.arange(0, pixel_count, stride, dtype=np.int64)
            sampled = values[indices]

            x = indices % width
            column_indices = np.minimum(
                columns - 1,
                (x * columns // max(1, width)).astype(np.int64))

            waveform = np.zeros(
                (3, columns, bins),
                dtype=np.int32)

            # Keep the scope as a true 0..100 display. Values below 0 or above
            # 1 are ignored rather than clamped into the first/last bin, which
            # otherwise creates artificial flat lines at the footer/header.
            for channel in range(3):
                channel_values = sampled[:, channel]
                valid = (
                    np.isfinite(channel_values)
                    & (channel_values >= 0.0)
                    & (channel_values <= 1.0)
                )
                if not np.any(valid):
                    continue

                channel_bins = np.minimum(
                    bins - 1,
                    (channel_values[valid] * (bins - 1)).astype(np.int64))
                np.add.at(
                    waveform[channel],
                    (column_indices[valid], channel_bins),
                    1)

            luma = (
                0.2126 * sampled[:, 0]
                + 0.7152 * sampled[:, 1]
                + 0.0722 * sampled[:, 2])

            luma_waveform = np.zeros(
                (columns, bins),
                dtype=np.int32)

            valid_luma = (
                np.isfinite(luma)
                & (luma >= 0.0)
                & (luma <= 1.0)
            )
            if np.any(valid_luma):
                luma_bins = np.minimum(
                    bins - 1,
                    (luma[valid_luma] * (bins - 1)).astype(np.int64))
                np.add.at(
                    luma_waveform,
                    (column_indices[valid_luma], luma_bins),
                    1)

            # Preserve the sampled source pixels for the visible point-cloud
            # renderer. NumPy is available in the normal FilmViz runtime, so
            # without this assignment the waveform grid was populated but the
            # visible cloud had no points and therefore rendered empty.
            x_normalized = (
                x.astype(np.float32)
                / max(1.0, float(width - 1))
            )
            finite = np.all(np.isfinite(sampled), axis=1)
            cloud = np.column_stack((
                x_normalized[finite],
                sampled[finite, 0],
                sampled[finite, 1],
                sampled[finite, 2],
            ))

            self._waveform = waveform
            self._luma_waveform = luma_waveform
            self.update()
            return

        waveform = [
            [[0] * bins for _ in range(columns)]
            for _ in range(3)
        ]
        luma_waveform = [
            [0] * bins
            for _ in range(columns)
        ]
        cloud_points = []

        for pixel in range(0, pixel_count, stride):
            x = pixel % width
            column = min(
                columns - 1,
                int(x * columns / max(1, width)))
            offset = pixel * 3

            r = float(rgb[offset])
            g = float(rgb[offset + 1])
            b = float(rgb[offset + 2])

            if all(map(__import__("math").isfinite, (r, g, b))):
                cloud_points.append((
                    float(x) / max(1.0, float(width - 1)),
                    r, g, b))

            for channel, value in enumerate((r, g, b)):
                if not (0.0 <= value <= 1.0):
                    continue
                bin_index = min(
                    bins - 1,
                    int(value * (bins - 1)))
                waveform[channel][column][bin_index] += 1

            y = 0.2126 * r + 0.7152 * g + 0.0722 * b
            if 0.0 <= y <= 1.0:
                y_bin = min(
                    bins - 1,
                    int(y * (bins - 1)))
                luma_waveform[column][y_bin] += 1

        self._waveform = waveform
        self._luma_waveform = luma_waveform
        self.update()

    def set_mode(self, mode: str):
        self._mode = mode if mode in ("rgb", "y") else "rgb"
        self.update()

    def set_probe(self, u: float, v: float, rgb):
        self._probes.append((float(u), tuple(rgb)))
        self.update()

    def clear_probe(self):
        self._probes = []
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)
        painter.fillRect(self.rect(), QColor("#0b0d0f"))

        left_margin = 42
        right_margin = 12
        top_margin = 12
        label_height = 18
        bottom_margin = 12 + label_height

        plot = self.rect().adjusted(
            left_margin,
            top_margin,
            -right_margin,
            -bottom_margin)

        grid_color = QColor(75, 75, 75)
        minor_grid_color = QColor(42, 42, 42)
        label_color = QColor(185, 154, 36)

        painter.setPen(QPen(grid_color, 1.0))
        painter.drawRect(plot)

        # Restore the video-style 0..100 percentage scale. Pixel value 0 is
        # the bottom of the plot and 255 is the top.
        for percent in range(0, 101, 10):
            y = (
                plot.bottom()
                - (percent / 100.0)
                * plot.height())

            painter.setPen(
                QPen(
                    grid_color if percent % 20 == 0
                    else minor_grid_color,
                    1.0))

            painter.drawLine(
                QPointF(plot.left(), y),
                QPointF(plot.right(), y))

            painter.setPen(label_color)
            painter.drawText(
                QRectF(
                    2,
                    y - 8,
                    left_margin - 7,
                    16),
                Qt.AlignmentFlag.AlignRight
                | Qt.AlignmentFlag.AlignVCenter,
                str(percent))

        if self._waveform is None:
            painter.setPen(self.palette().color(self.foregroundRole()))
            painter.drawText(
                plot,
                Qt.AlignmentFlag.AlignCenter,
                "RGB waveform")
            return

        # Render the actual sampled pixels using the same proven approach as
        # the vectorscope: a small translucent core plus a fainter halo.
        # Repeated overlap naturally builds brightness and RGB overlap tends
        # toward white under additive blending.
        painter.setCompositionMode(
            QPainter.CompositionMode.CompositionMode_Plus)
        painter.setPen(Qt.PenStyle.NoPen)

        # Draw each occupied density bin once. The alpha is the exact opacity
        # obtained by compositing `count` samples with a fixed per-sample alpha:
        #
        #   A = 1 - (1 - a)^count
        #
        # This preserves density/opacity behavior without drawing every sample.
        base_alpha = 0.028

        if self._mode == "y":
            if self._luma_waveform is None:
                return

            data = self._luma_waveform
            columns = len(data)
            bins = len(data[0]) if columns else 0
            if columns <= 0 or bins <= 0:
                return

            cell_w = plot.width() / max(1, columns - 1)
            cell_h = plot.height() / max(1, bins - 1)

            for x_index, column in enumerate(data):
                px = plot.left() + x_index * cell_w

                for value, count in enumerate(column):
                    if count <= 0:
                        continue

                    alpha = 1.0 - ((1.0 - base_alpha) ** int(count))
                    alpha = min(0.78, alpha)

                    color = QColor(235, 235, 235)
                    color.setAlphaF(alpha)
                    painter.setBrush(color)

                    py = plot.bottom() - value * cell_h
                    painter.drawRect(
                        QRectF(
                            px,
                            py,
                            max(1.0, cell_w + 0.35),
                            max(1.0, cell_h + 0.35)))

            title = "Y waveform"

        else:
            colors = (
                QColor("#ef5555"),
                QColor("#55c477"),
                QColor("#5b8def"),
            )

            columns = len(self._waveform[0])
            bins = len(self._waveform[0][0]) if columns else 0
            if columns <= 0 or bins <= 0:
                return

            cell_w = plot.width() / max(1, columns - 1)
            cell_h = plot.height() / max(1, bins - 1)

            painter.setCompositionMode(
                QPainter.CompositionMode.CompositionMode_Plus)

            for channel, data in enumerate(self._waveform):
                base = colors[channel]

                for x_index, column in enumerate(data):
                    px = plot.left() + x_index * cell_w

                    for value, count in enumerate(column):
                        if count <= 0:
                            continue

                        alpha = 1.0 - ((1.0 - base_alpha) ** int(count))
                        alpha = min(0.72, alpha)

                        color = QColor(base)
                        color.setAlphaF(alpha)
                        painter.setBrush(color)

                        py = plot.bottom() - value * cell_h
                        painter.drawRect(
                            QRectF(
                                px,
                                py,
                                max(1.0, cell_w + 0.35),
                                max(1.0, cell_h + 0.35)))

            painter.setCompositionMode(
                QPainter.CompositionMode.CompositionMode_SourceOver)
            title = "RGB waveform"

        painter.setBrush(QColor(245, 210, 70))
        painter.setPen(QPen(QColor(245, 210, 70), 1.8))

        if self._mode == "y":
            for u, rgb in self._probes:
                y = (
                    0.2126 * rgb[0]
                    + 0.7152 * rgb[1]
                    + 0.0722 * rgb[2])
                px = plot.left() + u * plot.width()
                py = plot.bottom() - y * plot.height()
                painter.drawEllipse(QPointF(px, py), 4.5, 4.5)
        else:
            for u, rgb in self._probes:
                px = plot.left() + u * plot.width()
                for value in rgb:
                    py = plot.bottom() - value * plot.height()
                    painter.drawEllipse(QPointF(px, py), 4.5, 4.5)

        painter.setPen(self.palette().color(self.foregroundRole()))
        painter.drawText(
            QRectF(
                plot.left(),
                plot.bottom() + 2,
                plot.width(),
                label_height),
            Qt.AlignmentFlag.AlignCenter,
            title)


class ScopeCompareHost(QWidget):
    def __init__(self, result_widget: QWidget, reference_widget: QWidget):
        super().__init__()

        self.result_widget = result_widget
        self.reference_widget = reference_widget
        self._wipe = 0.5
        self._has_reference = False
        self._result_cache = QPixmap()
        self._reference_cache = QPixmap()
        self._cache_refresh_pending = False

        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)
        self.setMinimumSize(
            max(result_widget.minimumWidth(), reference_widget.minimumWidth()),
            max(result_widget.minimumHeight(), reference_widget.minimumHeight()))

        # The expensive scope widgets are rendered into cached pixmaps.
        # Moving the comparison wipe then becomes a cheap clipped pixmap draw
        # instead of repainting thousands of scope samples on every slider tick.
        self.result_widget.setParent(self)
        self.reference_widget.setParent(self)
        self.result_widget.hide()
        self.reference_widget.hide()

    def refresh_cache(self):
        # Never render child widgets synchronously from resizeEvent/paintEvent.
        # Qt/macOS can re-enter QWidget backing-store painting in that case.
        # Cache refreshes are deferred to the next event-loop turn instead.
        if self._cache_refresh_pending:
            return

        self._cache_refresh_pending = True
        QTimer.singleShot(0, self._rebuild_cache)

    def _rebuild_cache(self):
        self._cache_refresh_pending = False

        if self.width() <= 0 or self.height() <= 0 or not self.isVisible():
            return

        target = QRect(0, 0, self.width(), self.height())
        self.result_widget.setGeometry(target)
        self.reference_widget.setGeometry(target)

        result_cache = QPixmap(self.size())
        result_cache.fill(Qt.GlobalColor.transparent)
        self.result_widget.render(result_cache)

        if self._has_reference:
            reference_cache = QPixmap(self.size())
            reference_cache.fill(Qt.GlobalColor.transparent)
            self.reference_widget.render(reference_cache)
        else:
            reference_cache = QPixmap()

        self._result_cache = result_cache
        self._reference_cache = reference_cache
        self.update()

    def set_reference_enabled(self, enabled: bool):
        self._has_reference = bool(enabled)
        self.refresh_cache()

    def set_wipe(self, value: int):
        self._wipe = min(1.0, max(0.0, float(value) / 100.0))
        # Intentionally do not rebuild scope geometry here. The cached result
        # and reference images are simply recomposited at the new divider.
        self.update()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.refresh_cache()

    def showEvent(self, event):
        super().showEvent(event)
        self.refresh_cache()

    def paintEvent(self, event):
        painter = QPainter(self)

        if not self._result_cache.isNull():
            painter.drawPixmap(0, 0, self._result_cache)

        if self._has_reference and not self._reference_cache.isNull():
            wipe_x = int(round(self.width() * self._wipe))
            wipe_x = min(self.width(), max(0, wipe_x))

            if wipe_x > 0:
                painter.save()
                painter.setClipRect(QRect(0, 0, wipe_x, self.height()))
                painter.drawPixmap(0, 0, self._reference_cache)
                painter.restore()

            painter.setPen(QPen(QColor(245, 210, 70), 1.0))
            divider_x = min(
                max(0, wipe_x),
                max(0, self.width() - 1))
            painter.drawLine(
                QPointF(divider_x, 0),
                QPointF(divider_x, self.height()))


class ScopePane(QWidget):
    popOutRequested = Signal(object)

    def __init__(self, initial: str):
        super().__init__()

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(5)

        toolbar = QWidget()
        toolbar_layout = QHBoxLayout(toolbar)
        toolbar_layout.setContentsMargins(0, 0, 0, 0)
        toolbar_layout.setSpacing(6)

        self.selector = QComboBox()
        self.selector.addItems((
            "Vectorscope",
            "RGB Histogram",
            "RGB Parade",
            "RGB Waveform",
            "Y Waveform",
        ))
        self.selector.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Fixed)

        self.zoom2 = QCheckBox("×2")
        self.zoom2.setToolTip(
            "Magnify vectorscope chroma displacement by 2×.")
        self.zoom2.setChecked(False)

        self.pop_out_button = QPushButton("↗")
        self.pop_out_button.setFixedSize(28, 24)
        self.pop_out_button.setToolTip("Open this scope in a separate window")
        self.pop_out_button.clicked.connect(
            lambda: self.popOutRequested.emit(self))

        toolbar_layout.addWidget(self.selector, 1)
        toolbar_layout.addWidget(self.zoom2, 0)
        toolbar_layout.addWidget(self.pop_out_button, 0)

        self.stack = QStackedWidget()
        self.stack.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)

        self.vector_scope = VectorScopeWidget()
        self.histogram = HistogramWidget()
        self.parade = ParadeWidget()
        self.waveform = WaveformWidget()

        self.reference_vector_scope = VectorScopeWidget()
        self.reference_histogram = HistogramWidget()
        self.reference_parade = ParadeWidget()
        self.reference_waveform = WaveformWidget()

        self.vector_host = ScopeCompareHost(
            self.vector_scope,
            self.reference_vector_scope)
        self.histogram_host = ScopeCompareHost(
            self.histogram,
            self.reference_histogram)
        self.parade_host = ScopeCompareHost(
            self.parade,
            self.reference_parade)
        self.waveform_host = ScopeCompareHost(
            self.waveform,
            self.reference_waveform)

        self._scope_hosts = (
            self.vector_host,
            self.histogram_host,
            self.parade_host,
            self.waveform_host,
        )

        self.stack.addWidget(self.vector_host)
        self.stack.addWidget(self.histogram_host)
        self.stack.addWidget(self.parade_host)
        self.stack.addWidget(self.waveform_host)

        layout.addWidget(toolbar)
        layout.addWidget(self.stack, 1)

        index = self.selector.findText(initial)
        if index >= 0:
            self.selector.setCurrentIndex(index)

        self.selector.currentIndexChanged.connect(
            self._mode_changed)
        self.zoom2.toggled.connect(
            self.vector_scope.set_zoom2)
        self.zoom2.toggled.connect(
            self.reference_vector_scope.set_zoom2)
        self.zoom2.toggled.connect(
            self._refresh_scope_caches)

        self._mode_changed(
            self.selector.currentIndex())

    def _mode_changed(self, index: int):
        if index == 4:
            self.stack.setCurrentIndex(3)
            self.waveform.set_mode("y")
            self.reference_waveform.set_mode("y")
        else:
            self.stack.setCurrentIndex(index)
            if index == 3:
                self.waveform.set_mode("rgb")
                self.reference_waveform.set_mode("rgb")

        self.zoom2.setVisible(index == 0)
        self._refresh_scope_caches()

    def _refresh_scope_caches(self):
        for host in self._scope_hosts:
            host.refresh_cache()

    def set_rgb(self, width: int, height: int, rgb):
        # Populate every result scope once so changing the dropdown is instant.
        self.vector_scope.set_rgb(width, height, rgb)
        self.histogram.set_rgb(width, height, rgb)
        self.parade.set_rgb(width, height, rgb)
        self.waveform.set_rgb(width, height, rgb)
        self._refresh_scope_caches()

    def set_reference_rgb(self, width: int, height: int, rgb):
        # Reference scopes use the same scope implementations and are clipped
        # by the exact same wipe position as the image comparison.
        self.reference_vector_scope.set_rgb(width, height, rgb)
        self.reference_histogram.set_rgb(width, height, rgb)
        self.reference_parade.set_rgb(width, height, rgb)
        self.reference_waveform.set_rgb(width, height, rgb)

        for host in self._scope_hosts:
            host.set_reference_enabled(True)

    def set_reference_wipe(self, value: int):
        for host in self._scope_hosts:
            host.set_wipe(value)

    def clear_reference(self):
        for host in self._scope_hosts:
            host.set_reference_enabled(False)

    def set_probe(self, u: float, v: float, rgb):
        # The picked value remains a FilmViz/result probe. The reference side
        # is comparison-only and does not alter pipeline probing.
        self.vector_scope.set_probe(u, v, rgb)
        self.histogram.set_probe(u, v, rgb)
        self.parade.set_probe(u, v, rgb)
        self.waveform.set_probe(u, v, rgb)
        self._refresh_scope_caches()

    def set_probes(self, probes):
        self.vector_scope.clear_probe()
        self.histogram.clear_probe()
        self.parade.clear_probe()
        self.waveform.clear_probe()
        for u, v, rgb in probes:
            self.vector_scope.set_probe(u, v, rgb)
            self.histogram.set_probe(u, v, rgb)
            self.parade.set_probe(u, v, rgb)
            self.waveform.set_probe(u, v, rgb)
        self._refresh_scope_caches()

    def clear_probe(self):
        self.vector_scope.clear_probe()
        self.histogram.clear_probe()
        self.parade.clear_probe()
        self.waveform.clear_probe()
        self._refresh_scope_caches()


class FloatingScopeWindow(QWidget):
    splitChanged = Signal(bool)
    closing = Signal()

    def __init__(self, primary_scope: ScopePane, parent=None):
        flags = (
            Qt.WindowType.Tool
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.WindowCloseButtonHint
            | Qt.WindowType.WindowMinMaxButtonsHint
        )
        super().__init__(parent, flags)
        self.setWindowTitle("FilmViz Scopes")
        self.resize(920, 600)
        self.setMinimumSize(520, 360)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(6)

        toolbar = QWidget()
        toolbar_layout = QHBoxLayout(toolbar)
        toolbar_layout.setContentsMargins(0, 0, 0, 0)
        toolbar_layout.setSpacing(6)

        toolbar_layout.addWidget(QLabel("Scopes"), 0)
        toolbar_layout.addStretch(1)

        self.split_button = QPushButton("2 scopes")
        self.split_button.setCheckable(True)
        self.split_button.setToolTip(
            "Show one or two scope panes in this window")
        self.split_button.toggled.connect(self.splitChanged.emit)
        toolbar_layout.addWidget(self.split_button, 0)

        layout.addWidget(toolbar, 0)

        self.scope_splitter = QSplitter(Qt.Orientation.Horizontal)
        self.scope_splitter.setChildrenCollapsible(False)
        layout.addWidget(self.scope_splitter, 1)

        self.primary_scope = primary_scope
        self.secondary_scope = None
        self.add_scope(primary_scope)

    def add_scope(self, pane: ScopePane):
        pane.setParent(None)
        self.scope_splitter.addWidget(pane)
        pane.show()
        pane.pop_out_button.hide()

        if pane is not self.primary_scope:
            self.secondary_scope = pane

        if self.scope_splitter.count() == 2:
            self.scope_splitter.setStretchFactor(0, 1)
            self.scope_splitter.setStretchFactor(1, 1)
            width = max(2, self.scope_splitter.width())
            self.scope_splitter.setSizes([width // 2, width // 2])

    def remove_secondary(self):
        pane = self.secondary_scope
        if pane is None:
            return None
        pane.setParent(None)
        self.secondary_scope = None
        return pane

    def closeEvent(self, event):
        self.closing.emit()
        event.accept()


class FilmVizWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self._memory_log = create_memory_log()
        self.setWindowTitle("FilmViz — Experimental Spectral Film Processor")
        self.resize(1500, 860)
        self.setMinimumSize(1180, 700)
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
        self._scope_width = 0
        self._scope_height = 0
        self._scope_rgb = None
        self._reference_scope_width = 0
        self._reference_scope_height = 0
        self._reference_scope_rgb = None
        self._reference_image_filename = None
        self._probe_points = []
        self._probe_results = []
        self._probe_source_identity = None
        self._runtime_resources = None
        self._runtime_resources_source = None
        self._profile_edit_path = None
        self._profile_edit_x_column = None
        self._floating_scope_window = None
        self._floating_scope_primary = None
        self._floating_scope_secondary = None
        self._metal_preview = None
        self._metal_preview_busy = False
        self._metal_preview_pending = False
        self._metal_preview_generation = 0
        self._metal_profiles_dirty = False
        self._metal_preview_bridge = MetalPreviewBridge(self)
        self._metal_preview_bridge.finished.connect(
            self._metal_preview_finished)
        self._metal_preview_bridge.failed.connect(
            self._metal_preview_failed)
        self._metal_preview_timer = QTimer(self)
        self._metal_preview_timer.setSingleShot(True)
        self._metal_preview_timer.setInterval(75)
        self._metal_preview_timer.timeout.connect(
            self._start_metal_preview)
        if self._memory_log is not None:
            self._memory_timer = QTimer(self)
            self._memory_timer.setInterval(2000)
            self._memory_timer.timeout.connect(self._sample_preview_memory)
            self._memory_timer.start()

        central = QWidget()
        central_layout = QHBoxLayout(central)
        central_layout.setContentsMargins(8, 8, 8, 8)
        self.setCentralWidget(central)

        workspace = QSplitter(Qt.Orientation.Horizontal)
        central_layout.addWidget(workspace)

        diagnostics = QWidget()
        diagnostics_layout = QVBoxLayout(diagnostics)
        diagnostics_layout.setContentsMargins(0, 0, 0, 0)

        display_toolbar = QWidget()
        display_toolbar_layout = QGridLayout(display_toolbar)
        display_toolbar_layout.setContentsMargins(0, 0, 0, 0)
        display_toolbar_layout.setHorizontalSpacing(6)

        reference_controls = QWidget()
        reference_controls_layout = QHBoxLayout(reference_controls)
        reference_controls_layout.setContentsMargins(0, 0, 0, 0)
        reference_controls_layout.setSpacing(8)

        self.reference_icon = QLabel()
        self.reference_icon.setPixmap(
            _reference_image_icon(18).pixmap(QSize(18, 18)))
        self.reference_icon.setFixedSize(24, 24)
        self.reference_icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.reference_icon.setToolTip(
            "Drop a reference image onto the converted-image preview.")

        self.reference_wipe = QSlider(Qt.Orientation.Horizontal)
        self.reference_wipe.setRange(0, 100)
        self.reference_wipe.setValue(50)
        self.reference_wipe.setFixedWidth(360)
        self.reference_wipe.setEnabled(False)
        self.reference_wipe.setToolTip(
            "Wipe between the reference image and the FilmViz result. "
            "The same divider is used by all scopes.")

        self.clear_reference_button = QPushButton()
        self.clear_reference_button.setIcon(_trash_icon(18))
        self.clear_reference_button.setIconSize(QSize(18, 18))
        self.clear_reference_button.setFixedSize(30, 28)
        self.clear_reference_button.setEnabled(False)
        self.clear_reference_button.setToolTip("Remove the reference image")

        reference_controls_layout.addWidget(self.reference_icon, 0)
        reference_controls_layout.addWidget(self.reference_wipe, 0)
        reference_controls_layout.addWidget(self.clear_reference_button, 0)

        display_label = QLabel("Display: Rec.709 Gamma 2.4")
        display_label.setToolTip(
            "FilmViz preview and application surface are configured for "
            "Rec.709 Gamma 2.4.")

        # Equal outer columns keep the reference controls geometrically
        # centered even though the display label lives at the far right.
        display_toolbar_layout.setColumnStretch(0, 1)
        display_toolbar_layout.setColumnStretch(2, 1)
        display_toolbar_layout.addWidget(
            reference_controls,
            0, 1,
            Qt.AlignmentFlag.AlignCenter)
        display_toolbar_layout.addWidget(
            display_label,
            0, 2,
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        diagnostics_layout.addWidget(display_toolbar, 0)

        diagnostics_splitter = QSplitter(Qt.Orientation.Vertical)
        diagnostics_splitter.setChildrenCollapsible(False)

        self.image_preview = ImagePreviewWidget()
        self.image_preview.probeRequested.connect(
            self._probe_image_pixel)
        self.reference_wipe.valueChanged.connect(
            self.image_preview.set_reference_wipe)
        self.clear_reference_button.clicked.connect(
            self._clear_reference_image)
        diagnostics_splitter.addWidget(self.image_preview)

        self.probe_output = QPlainTextEdit()
        self.probe_output.setReadOnly(True)
        self.probe_output.setMinimumHeight(70)
        fixed_font = QFontDatabase.systemFont(
            QFontDatabase.SystemFont.FixedFont)
        self.probe_output.setFont(fixed_font)
        self.probe_output.setPlaceholderText(
            "Use Pick color, then click the converted image to inspect the "
            "matching source pixel through AP0 → spectrum → negative → print → output.")
        diagnostics_splitter.addWidget(self.probe_output)

        self.scopes_splitter = QSplitter(Qt.Orientation.Horizontal)
        self.left_scope = ScopePane("Vectorscope")
        self.right_scope = ScopePane("RGB Histogram")
        self.reference_wipe.valueChanged.connect(
            self.left_scope.set_reference_wipe)
        self.reference_wipe.valueChanged.connect(
            self.right_scope.set_reference_wipe)
        self.left_scope.popOutRequested.connect(
            self._pop_out_scope)
        self.right_scope.popOutRequested.connect(
            self._pop_out_scope)
        self.scopes_splitter.addWidget(self.left_scope)
        self.scopes_splitter.addWidget(self.right_scope)
        self.scopes_splitter.setStretchFactor(0, 1)
        self.scopes_splitter.setStretchFactor(1, 1)
        self.scopes_splitter.setChildrenCollapsible(False)
        diagnostics_splitter.addWidget(self.scopes_splitter)

        diagnostics_splitter.setStretchFactor(0, 4)
        diagnostics_splitter.setStretchFactor(1, 1)
        diagnostics_splitter.setStretchFactor(2, 3)
        diagnostics_splitter.setSizes([470, 110, 290])
        diagnostics_layout.addWidget(diagnostics_splitter, 1)

        panel = QWidget()
        panel.setMinimumWidth(390)
        panel.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)
        root_layout = QVBoxLayout(panel)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(6)

        panel.setStyleSheet(
            "QGroupBox { margin-top: 6px; }"
            "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {"
            "  min-height: 20px;"
            "  max-height: 24px;"
            "}"
        )

        workspace.addWidget(diagnostics)
        workspace.addWidget(panel)
        workspace.setChildrenCollapsible(False)
        workspace.setStretchFactor(0, 5)
        workspace.setStretchFactor(1, 1)
        workspace.setSizes([1180, 420])

        profiles = filmviz.profiles(str(PROJECT_ROOT / "resources"))
        self.negative_profiles = [
            dict(profile)
            for profile in profiles["negative_details"]
        ]
        self.negative_profiles_by_id = {
            profile["identifier"]: profile
            for profile in self.negative_profiles
        }
        self.print_profiles = [
            dict(profile)
            for profile in profiles["print_details"]
        ]
        self.print_profiles_by_id = {
            profile["identifier"]: profile
            for profile in self.print_profiles
        }
        self.film_formats = [
            dict(format_entry)
            for format_entry in profiles["film_formats"]
        ]
        self.film_formats_by_id = {
            format_entry["identifier"]: format_entry
            for format_entry in self.film_formats
        }
        common = QWidget()
        common.setMinimumWidth(360)
        common.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Maximum)

        common_layout = QVBoxLayout(common)
        common_layout.setContentsMargins(0, 0, 0, 0)
        common_layout.setSpacing(6)

        pipeline_header = QWidget()
        pipeline_header_layout = QHBoxLayout(pipeline_header)
        pipeline_header_layout.setContentsMargins(0, 0, 0, 0)
        pipeline_header_layout.addStretch(1)
        self.pipeline_reset_button = _reset_button()
        pipeline_header_layout.addWidget(self.pipeline_reset_button)

        common_form_widget = QWidget()
        common_form = QFormLayout(common_form_widget)
        common_layout.addWidget(pipeline_header)
        common_layout.addWidget(common_form_widget)
        common_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        self.resources = PathRow(
            "directory",
            str(PROJECT_ROOT / "resources"),
            minimum_width=0)
        common_form.addRow("Resource directory", self.resources)
        self._preset_settings = QSettings("FilmViz", "FilmViz")
        self.preset_selector = QComboBox()
        preset_row = QWidget()
        preset_layout = QHBoxLayout(preset_row)
        preset_layout.setContentsMargins(0, 0, 0, 0)
        preset_layout.addWidget(self.preset_selector, 1)
        for label, callback in (("Save…", self._save_preset),
                                ("Load", self._load_preset),
                                ("Delete", self._delete_preset)):
            button = QPushButton(label)
            button.clicked.connect(callback)
            preset_layout.addWidget(button)
        preset_row.setToolTip("App-local look presets. Image paths and measured profile edits are not stored.")
        common_form.addRow("Preset", preset_row)
        self._refresh_presets()

        self.input_profile = ColorProfileSelector(profiles["input_details"], self)
        self.input_profile.setCurrentText("ARRI LogC3 (EI800)")
        common_form.addRow("Input Color Space", self.input_profile.color_space)
        common_form.addRow("Input Transfer Function", self.input_profile.transfer_function)

        self.negative_profile = QComboBox()
        for profile in self.negative_profiles:
            self.negative_profile.addItem(
                profile["display_name"],
                profile["identifier"])
        common_form.addRow("Negative", self.negative_profile)

        self.print_profile = QComboBox()
        for profile in self.print_profiles:
            self.print_profile.addItem(
                profile["display_name"],
                profile["identifier"])
        self.print_profile.addItem("None — view negative", "none")
        common_form.addRow("Print", self.print_profile)

        self.output_profile = ColorProfileSelector(profiles["output_details"], self)
        self.output_profile.setCurrentText("rec709-gamma24")
        common_form.addRow("Output Color Space", self.output_profile.color_space)
        common_form.addRow("Output Transfer Function", self.output_profile.transfer_function)

        self.realtime_metal = QCheckBox()
        metal_available = bool(
            getattr(filmviz, "metal_preview_available", False))
        self.realtime_metal.setEnabled(metal_available)
        self.realtime_metal.setChecked(False)
        self.realtime_metal.setToolTip(
            "Render the current input through the direct Metal pipeline after "
            "each control or runtime profile edit."
            if metal_available else
            "Realtime Metal preview is unavailable in this build.")
        common_form.addRow("Realtime Metal preview", self.realtime_metal)

        self.exposure = _double(0.0, -10.0, 10.0, 0.25)
        self.negative_flash = _double(0.0, 0.0, 25.0, 0.1, 2)
        self.print_flash = _double(0.0, 0.0, 25.0, 0.1, 2)
        self.push_pull = _double(0.0, -5.0, 5.0, 0.25)
        self.color_density = _double(0.0, -4.0, 4.0, 0.1, 2)
        self.color_density.setToolTip(
            "Controls neutral-preserving chroma separation. "
            "Minus four bypasses Color Response; zero is standard.")
        self.color_depth = _double(1.0, -1.0, 2.0, 0.05, 2)
        self.color_depth.setToolTip(
            "Controls chroma-weighted density independently. "
            "One is the current response, zero removes color darkening, "
            "and negative values lift chromatic regions.")
        self.negative_bleach_bypass = _double(0.0, 0.0, 1.0, 0.05)
        self.print_bleach_bypass = _double(0.0, 0.0, 1.0, 0.05)
        self.printer_light_red = _double(25.0, 0.0, 50.0, 0.1, 1)
        self.printer_light_green = _double(25.0, 0.0, 50.0, 0.1, 1)
        self.printer_light_blue = _double(25.0, 0.0, 50.0, 0.1, 1)
        self.printer_light_master = _double(0.0, -10.0, 10.0, 0.05, 2)
        self.middle_gray = _double(0.18, 0.001, 2.0, 0.01, 4)
        self.printer_temperature = _double(3200.0, 1000.0, 10000.0, 50.0, 0)
        self.lut_size = DragSpinBox()
        self.lut_size.setRange(2, 129)
        self.lut_size.setValue(65)
        self.use_lut_acceleration = QCheckBox()
        self.use_lut_acceleration.setChecked(True)
        self.use_lut_acceleration.setToolTip(
            "Disable to evaluate the spectral FilmPipeline directly per pixel. "
            "Direct mode is intended for validation and requires grain and "
            "halation to be disabled.")
        self.threads = DragSpinBox()
        self.threads.setRange(0, 256)
        self.threads.setSpecialValueText("Auto")
        self.threads.setValue(0)

        self.exposure_control = SliderSpinRow(self.exposure)
        self.negative_flash_control = SliderSpinRow(self.negative_flash)
        self.print_flash_control = SliderSpinRow(self.print_flash)
        self.push_pull_control = SliderSpinRow(self.push_pull)
        self.color_density_control = SliderSpinRow(self.color_density)
        self.color_depth_control = SliderSpinRow(self.color_depth)
        self.negative_bleach_bypass_control = SliderSpinRow(
            self.negative_bleach_bypass)
        self.print_bleach_bypass_control = SliderSpinRow(
            self.print_bleach_bypass)
        self.printer_light_red_control = SliderSpinRow(
            self.printer_light_red)
        self.printer_light_green_control = SliderSpinRow(
            self.printer_light_green)
        self.printer_light_blue_control = SliderSpinRow(
            self.printer_light_blue)
        self.printer_light_master_control = SliderSpinRow(
            self.printer_light_master)

        for widget in (
            self.exposure,
            self.negative_flash,
            self.print_flash,
            self.push_pull,
            self.color_density,
            self.color_depth,
            self.negative_bleach_bypass,
            self.print_bleach_bypass,
            self.printer_light_red,
            self.printer_light_green,
            self.printer_light_blue,
            self.printer_light_master,
            self.middle_gray,
            self.printer_temperature,
            self.lut_size,
            self.threads,
        ):
            widget.setMinimumWidth(72)

        controls = QWidget()
        controls.setMinimumWidth(330)
        controls.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Preferred)
        controls_layout = QGridLayout(controls)
        controls_layout.setContentsMargins(0, 0, 0, 0)
        controls_layout.setHorizontalSpacing(8)
        controls_layout.setVerticalSpacing(4)

        for row, (label, widget) in enumerate((
            ("Exposure stops", self.exposure_control),
            ("Negative flash (%)", self.negative_flash_control),
            ("Print flash (%)", self.print_flash_control),
            ("Push/pull stops", self.push_pull_control),
            ("Negative bypass", self.negative_bleach_bypass_control),
            ("Print bypass", self.print_bleach_bypass_control),
            ("Printer R light", self.printer_light_red_control),
            ("Printer G light", self.printer_light_green_control),
            ("Printer B light", self.printer_light_blue_control),
            ("Printer master", self.printer_light_master_control),
            ("Printer K", self.printer_temperature),
            ("Middle gray", self.middle_gray),
            ("LUT size", self.lut_size),
            ("Use LUT acceleration", self.use_lut_acceleration),
            ("Worker threads", self.threads),
        )):
            label_widget = QLabel(label)
            label_widget.setAlignment(
                Qt.AlignmentFlag.AlignRight
                | Qt.AlignmentFlag.AlignVCenter)
            controls_layout.addWidget(label_widget, row, 0)
            controls_layout.addWidget(widget, row, 1)

        controls_layout.setColumnStretch(0, 0)
        controls_layout.setColumnStretch(1, 1)
        common_form.addRow(controls)
        self.print_profile.currentIndexChanged.connect(
            self._print_profile_changed)

        controls_splitter = QSplitter(Qt.Orientation.Vertical)
        controls_splitter.setChildrenCollapsible(False)

        pipeline_tabs = QTabWidget()
        pipeline_tabs.setMinimumWidth(360)
        pipeline_tabs.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)

        pipeline_page = QWidget()
        pipeline_page_layout = QVBoxLayout(pipeline_page)
        pipeline_page_layout.setContentsMargins(0, 0, 0, 0)

        pipeline_scroll = QScrollArea()
        pipeline_scroll.setWidgetResizable(True)
        pipeline_scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        pipeline_scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        pipeline_scroll.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded)

        pipeline_scroll_host = QWidget()
        pipeline_scroll_host_layout = QHBoxLayout(pipeline_scroll_host)
        pipeline_scroll_host_layout.setContentsMargins(0, 0, 0, 0)
        pipeline_scroll_host_layout.addWidget(
            common,
            1,
            Qt.AlignmentFlag.AlignTop)

        pipeline_scroll.setWidget(pipeline_scroll_host)
        pipeline_page_layout.addWidget(pipeline_scroll)
        pipeline_tabs.addTab(pipeline_page, "Pipeline")

        response_page = QWidget()
        response_layout = QFormLayout(response_page)
        response_note = QLabel("Empirical negative dye shaping. Reset restores the current look.")
        response_note.setWordWrap(True)
        response_layout.addRow(response_note)
        self.response_bypass = QCheckBox("Bypass Color Response")
        response_layout.addRow(self.response_bypass)
        self.response_controls = {}
        self.response_rows = {}
        self.response_defaults = {}
        for name, label, default, minimum, maximum, step, tooltip in (
            ('response_amount', 'Response amount', 1.0, 0.0, 1.0, 0.05, 'Blend from calibrated bypass (0) to the complete shaped response (1).'),
            ('chroma_compression', 'Chroma compression', 0.22, 0.0, 1.0, 0.01, 'Compression of normalized negative dye-channel differences.'),
            ('chroma_knee', 'Chroma knee', 0.5, 0.05, 2.0, 0.05, 'Chroma scale of compression and density depth; smaller values act earlier.'),
            ('density_center', 'Density center', 1.25, 0.0, 3.0, 0.05, 'Center of the shaping envelope in normalized negative dye density, not image luminance.'),
            ('density_width', 'Density width', 1.0, 0.25, 3.0, 0.05, 'Scale of the density envelope and its smooth low/high-density fades.'),
            ('warm_protection', 'Warm protection', 0.5, 0.0, 1.0, 0.05, 'Maximum reduction in chroma compression within the selected warm region.'),
            ('warm_hue_center', 'Warm hue center (°)', 0.0, -180.0, 180.0, 1.0, 'Dye-plane angle relative to yellow: positive toward red, negative toward green.'),
            ('warm_hue_width', 'Warm hue width', 1.0, 0.25, 3.0, 0.05, 'Width of the selected hue region; one preserves the current selection.'),
            ('warm_hue_shift', 'Warm hue shift (°)', 0.0, -45.0, 45.0, 1.0, 'Additional localized rotation: positive toward red, negative toward green. Fades outside the selected region.'),
        ):
            control = _double(default, minimum, maximum, step, 2)
            control.setToolTip(tooltip)
            self.response_controls[name] = control
            self.response_defaults[name] = default
            self.response_rows[name] = SliderSpinRow(control)
            response_layout.addRow(label, self.response_rows[name])
        response_layout.insertRow(5, "Color depth", self.color_depth_control)
        self.response_reset = QPushButton("Reset Color Response")
        response_layout.addRow(self.response_reset)
        self.response_plot = ColorResponsePlot()
        response_layout.addRow(self.response_plot)
        response_scroll = QScrollArea()
        response_scroll.setWidgetResizable(True)
        response_scroll.setWidget(response_page)
        pipeline_tabs.addTab(response_scroll, "Color Response")

        controls_splitter.addWidget(pipeline_tabs)

        self.tabs = QTabWidget()
        self.tabs.setMinimumWidth(360)
        self.tabs.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)
        controls_splitter.addWidget(self.tabs)

        controls_splitter.setStretchFactor(0, 1)
        controls_splitter.setStretchFactor(1, 2)
        controls_splitter.setSizes([330, 470])
        root_layout.addWidget(controls_splitter, 1)

        image_tab = QWidget()
        image_tab.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Maximum)

        image_tab_layout = QVBoxLayout(image_tab)
        image_tab_layout.setContentsMargins(0, 0, 0, 0)
        image_tab_layout.setSpacing(6)

        image_header = QWidget()
        image_header_layout = QHBoxLayout(image_header)
        image_header_layout.setContentsMargins(0, 0, 0, 0)
        image_header_layout.addStretch(1)
        self.image_reset_button = _reset_button()
        image_header_layout.addWidget(self.image_reset_button)

        image_form_widget = QWidget()
        image_form = QFormLayout(image_form_widget)
        image_tab_layout.addWidget(image_header)
        image_tab_layout.addWidget(image_form_widget)
        image_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        default_image = PROJECT_ROOT / "resources" / "references" / "images" / "ARRI_Helen_John_ALEXA_Mini_LF_AWG3_LogC3.tif"
        self.input_image = PathRow(
            "input",
            str(default_image),
            minimum_width=0)
        self.output_image = PathRow(
            "output",
            str(PROJECT_ROOT / "build" / "filmviz_output.tif"),
            minimum_width=0)
        image_form.addRow("Input image", self.input_image)
        image_form.addRow("Output image", self.output_image)
        self.negative_grain = _double(0.0, 0.0, 2.0)
        self.print_grain = _double(0.0, 0.0, 2.0)
        self.grain_size = _double(1.0, 0.25, 10.0, 0.25)
        self.grain_size.setToolTip("Grain size multiplier referenced to 2048-pixel-wide Super 35. Both texture bands scale with film format; pixel-area integration handles reduced previews. Strength uses the experimental 48 µm aperture normalization.")
        self.grain_chroma = _double(1.0, 0.0, 1.0, 0.1)
        self.grain_enabled = QCheckBox("Enable grain")
        self.grain_enabled.setChecked(True)
        self.grain_enabled.setToolTip(
            "Bypass negative and print grain for A/B comparison without changing their settings.")
        self.halation_enabled = QCheckBox("Enable halation")
        self.halation_enabled.setChecked(True)
        self.halation_enabled.setToolTip(
            "Bypass halation for A/B comparison without changing its settings.")
        self.grain_tonal_enabled = QCheckBox("Enable tonal grain shaping")
        self.grain_tonal_enabled.setChecked(True)
        self.grain_tonal_enabled.setToolTip(
            "Empirical rendering control. Uncheck to bypass tonal attenuation; "
            "measured profiles and other grain controls remain active.")
        self.grain_shadows = _double(1.0, 0.0, 2.0, 0.05)
        self.grain_midtones = _double(1.0, 0.0, 2.0, 0.05)
        self.grain_highlights = _double(1.0, 0.0, 2.0, 0.05)
        self.grain_seed = DragSpinBox()
        self.grain_seed.setRange(0, 2_147_483_647)
        self.grain_seed.setValue(1)
        self.film_format = QComboBox()
        for format_entry in self.film_formats:
            self.film_format.addItem(
                format_entry["display_name"],
                format_entry["identifier"])
        self.film_format.setCurrentIndex(
            self.film_format.findData("super-35"))
        self.image_width_mm = _double(24.89, 1.0, 100.0, 0.01, 2)
        self.negative_mtf = _double(0.0, 0.0, 100.0, 0.1, 1)
        self.print_mtf = _double(0.0, 0.0, 100.0, 0.1, 1)
        self.negative_mtf.setSuffix(" %")
        self.print_mtf.setSuffix(" %")
        self.film_format.currentIndexChanged.connect(
            self._film_format_changed)
        self.halation_strength = _double(0.0, 0.0, 1.0, 0.05)
        self.halation_radius = _double(12.0, 0.0, 200.0, 1.0, 1)
        self.halation_threshold = _double(0.7, 0.0, 4.0, 0.05, 3)

        self.negative_grain_control = SliderSpinRow(
            self.negative_grain)
        self.print_grain_control = SliderSpinRow(
            self.print_grain)
        self.grain_size_control = SliderSpinRow(
            self.grain_size)
        self.grain_chroma_control = SliderSpinRow(
            self.grain_chroma)
        self.negative_mtf_control = SliderSpinRow(
            self.negative_mtf)
        self.print_mtf_control = SliderSpinRow(
            self.print_mtf)
        self.halation_strength_control = SliderSpinRow(
            self.halation_strength)
        self.halation_radius_control = SliderSpinRow(
            self.halation_radius)
        self.halation_threshold_control = SliderSpinRow(
            self.halation_threshold)
        image_form.addRow("Film format", self.film_format)
        image_form.addRow("Active image width (mm)", self.image_width_mm)
        image_form.addRow("Grain", self.grain_enabled)
        image_form.addRow("Negative grain", self.negative_grain_control)
        image_form.addRow("Print grain", self.print_grain_control)
        image_form.addRow("Grain scale (×)", self.grain_size_control)
        image_form.addRow("Grain chroma", self.grain_chroma_control)
        image_form.addRow("Grain rendering", self.grain_tonal_enabled)
        for label, spin in (("Shadow grain (×)", self.grain_shadows),
                            ("Midtone grain (×)", self.grain_midtones),
                            ("Highlight grain (×)", self.grain_highlights)):
            row = SliderSpinRow(spin)
            row.setToolTip("Multiplier over the current tonal grain look. 1 preserves it; 0 suppresses this range.")
            self.grain_tonal_enabled.toggled.connect(row.setEnabled)
            image_form.addRow(label, row)
        image_form.addRow("Grain seed", self.grain_seed)
        image_form.addRow("Negative MTF", self.negative_mtf_control)
        image_form.addRow("Print MTF", self.print_mtf_control)
        image_form.addRow("Halation", self.halation_enabled)
        image_form.addRow("Halation strength", self.halation_strength_control)
        image_form.addRow("Halation radius (px)", self.halation_radius_control)
        image_form.addRow("Halation threshold", self.halation_threshold_control)
        self.convert_button = QPushButton("Convert image")
        self.convert_button.clicked.connect(self.convert_image)

        self.convert_shortcut = QShortcut(
            QKeySequence(Qt.Key.Key_Return),
            self)
        self.convert_shortcut.setContext(
            Qt.ShortcutContext.ApplicationShortcut)
        self.convert_shortcut.activated.connect(
            self._convert_from_shortcut)

        self.convert_enter_shortcut = QShortcut(
            QKeySequence(Qt.Key.Key_Enter),
            self)
        self.convert_enter_shortcut.setContext(
            Qt.ShortcutContext.ApplicationShortcut)
        self.convert_enter_shortcut.activated.connect(
            self._convert_from_shortcut)

        self.open_output_button = QPushButton("Open output")
        self.open_output_button.clicked.connect(self.open_output_image)
        self.open_output_button.setVisible(False)
        self.open_output_button.setEnabled(False)
        image_scroll = QScrollArea()
        image_scroll.setWidgetResizable(True)
        image_scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        image_scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        image_scroll.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        image_scroll.setWidget(image_tab)

        self.tabs.addTab(image_scroll, "Convert Image")
        self._film_format_changed()

        lut_tab = QWidget()
        lut_tab.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Maximum)

        lut_tab_layout = QVBoxLayout(lut_tab)
        lut_tab_layout.setContentsMargins(0, 0, 0, 0)
        lut_tab_layout.setSpacing(6)

        lut_header = QWidget()
        lut_header_layout = QHBoxLayout(lut_header)
        lut_header_layout.setContentsMargins(0, 0, 0, 0)
        lut_header_layout.addStretch(1)
        self.lut_reset_button = _reset_button()
        lut_header_layout.addWidget(self.lut_reset_button)

        lut_form_widget = QWidget()
        lut_form = QFormLayout(lut_form_widget)
        lut_tab_layout.addWidget(lut_header)
        lut_tab_layout.addWidget(lut_form_widget)
        lut_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        self.output_lut = PathRow(
            "lut",
            str(PROJECT_ROOT / "build" / "filmviz.cube"),
            minimum_width=0)
        lut_form.addRow("Output LUT", self.output_lut)
        note = QLabel(
            "LUTs are deterministic and do not contain image grain or MTF.")
        note.setWordWrap(True)
        lut_form.addRow(note)
        self.lut_button = QPushButton("Generate LUT")
        self.lut_button.clicked.connect(self.generate_lut)
        lut_form.addRow(self.lut_button)
        lut_scroll = QScrollArea()
        lut_scroll.setWidgetResizable(True)
        lut_scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        lut_scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        lut_scroll.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        lut_scroll.setWidget(lut_tab)

        self.tabs.addTab(lut_scroll, "Generate LUT")


        profiles_tab = QWidget()
        profiles_tab.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Maximum)
        profiles_layout = QVBoxLayout(profiles_tab)

        profiles_header = QWidget()
        profiles_header_layout = QHBoxLayout(profiles_header)
        profiles_header_layout.setContentsMargins(0, 0, 0, 0)
        profiles_header_layout.addStretch(1)
        self.profile_save_button = QPushButton("Save profile…")
        self.profile_save_button.setEnabled(False)
        self.profile_save_button.setToolTip(
            "Export the current runtime-edited profile curves without changing "
            "the canonical resource CSV files.")
        self.profiles_reset_button = _reset_button()
        profiles_header_layout.addWidget(self.profile_save_button)
        profiles_header_layout.addWidget(self.profiles_reset_button)
        profiles_layout.addWidget(profiles_header)

        profile_controls = QWidget()
        profile_controls_layout = QHBoxLayout(profile_controls)
        profile_controls_layout.setContentsMargins(0, 0, 0, 0)

        self.profile_family = QComboBox()
        for profile in self.negative_profiles:
            self.profile_family.addItem(
                f"Negative — {profile['display_name']}",
                ("negative", profile["identifier"]))
        for profile in self.print_profiles:
            self.profile_family.addItem(
                f"Print — {profile['display_name']}",
                ("print", profile["identifier"]))
        self.profile_curve_type = QComboBox()
        self.profile_edit_spacing = QComboBox()
        self.profile_edit_spacing.addItem("All samples", 0.0)
        self.profile_edit_spacing.addItem("5 nm", 5.0)
        self.profile_edit_spacing.addItem("10 nm", 10.0)
        self.profile_edit_spacing.addItem("20 nm", 20.0)
        self.profile_edit_spacing.addItem("25 nm", 25.0)
        self.profile_edit_spacing.addItem("50 nm", 50.0)
        self.profile_edit_spacing.setToolTip(
            "Choose which wavelength samples are editable. Moving an anchor "
            "linearly interpolates samples between its neighboring anchors.")

        profile_controls_layout.addWidget(QLabel("Profile"))
        profile_controls_layout.addWidget(self.profile_family)
        profile_controls_layout.addSpacing(18)
        profile_controls_layout.addWidget(QLabel("Curves"))
        profile_controls_layout.addWidget(self.profile_curve_type, 1)
        profile_controls_layout.addSpacing(12)
        profile_controls_layout.addWidget(QLabel("Edit spacing"))
        profile_controls_layout.addWidget(self.profile_edit_spacing)

        self.profile_plot = CurvePlotWidget()

        point_editor = QWidget()
        point_editor_layout = QHBoxLayout(point_editor)
        point_editor_layout.setContentsMargins(0, 0, 0, 0)
        point_editor_layout.setSpacing(8)

        self.profile_selected_curve = QLabel("No point selected")
        self.profile_selected_curve.setMinimumWidth(120)

        self.profile_point_x = DragDoubleSpinBox()
        self.profile_point_x.setDecimals(8)
        self.profile_point_x.setRange(-1.0e9, 1.0e9)
        self.profile_point_x.setReadOnly(True)
        self.profile_point_x.setButtonSymbols(QDoubleSpinBox.ButtonSymbols.NoButtons)
        self.profile_point_x.setEnabled(False)

        self.profile_point_y = DragDoubleSpinBox()
        self.profile_point_y.setDecimals(8)
        self.profile_point_y.setRange(-1.0e9, 1.0e9)
        self.profile_point_y.setSingleStep(0.001)
        self.profile_point_y.setEnabled(False)

        self.profile_reset_curve_button = QPushButton("Reset curve")
        self.profile_reset_curve_button.setEnabled(False)
        self.profile_reset_profile_button = QPushButton("Reset profile")
        self.profile_reset_profile_button.setEnabled(False)

        point_editor_layout.addWidget(QLabel("Selected"))
        point_editor_layout.addWidget(self.profile_selected_curve, 1)
        point_editor_layout.addWidget(QLabel("X"))
        point_editor_layout.addWidget(self.profile_point_x)
        point_editor_layout.addWidget(QLabel("Y"))
        point_editor_layout.addWidget(self.profile_point_y)
        point_editor_layout.addWidget(self.profile_reset_curve_button)
        point_editor_layout.addWidget(self.profile_reset_profile_button)

        profile_hint = QLabel(
            "Click an editable sample to select it. Drag vertically or enter an exact Y value. "
            "For wavelength curves, Edit spacing can expose coarser nm anchors and interpolate "
            "the samples between neighboring anchors. Original CSV data remains untouched.")
        profile_hint.setWordWrap(True)

        profiles_layout.addWidget(profile_controls)
        profiles_layout.addWidget(self.profile_plot, 1)
        profiles_layout.addWidget(point_editor)
        profiles_layout.addWidget(profile_hint)

        self.profile_family.currentIndexChanged.connect(
            self._profile_family_changed)
        self.profile_curve_type.currentIndexChanged.connect(
            self._reload_profile_plot)
        self.profile_edit_spacing.currentIndexChanged.connect(
            self._profile_edit_spacing_changed)
        self.resources.edit.editingFinished.connect(
            self._resources_changed)
        self.profile_plot.pointSelected.connect(
            self._profile_point_selected)
        self.profile_plot.pointChanged.connect(
            self._profile_point_changed)
        self.profile_point_y.valueChanged.connect(
            self._profile_numeric_y_changed)
        self.profile_reset_curve_button.clicked.connect(
            self._reset_current_profile_curve)
        self.profile_reset_profile_button.clicked.connect(
            self._reset_current_profile)
        self.profile_save_button.clicked.connect(
            self._save_current_profile)

        profiles_scroll = QScrollArea()
        profiles_scroll.setWidgetResizable(True)
        profiles_scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        profiles_scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        profiles_scroll.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        profiles_scroll.setWidget(profiles_tab)

        self.tabs.addTab(profiles_scroll, "Profiles")
        self._profile_family_changed()
        self._print_profile_changed()

        self.pipeline_reset_button.clicked.connect(
            self._reset_pipeline)
        self.image_reset_button.clicked.connect(
            self._reset_image_settings)
        self.lut_reset_button.clicked.connect(
            self._reset_lut_settings)
        self.profiles_reset_button.clicked.connect(
            self._reset_profiles)

        self.realtime_metal.toggled.connect(
            self._realtime_metal_toggled)
        self.input_image.edit.editingFinished.connect(
            self._schedule_metal_preview)

        self.response_reset.clicked.connect(self._reset_color_response)
        self.response_bypass.toggled.connect(self._color_response_changed)
        for control in self.response_controls.values():
            control.valueChanged.connect(self._color_response_changed)
        self.color_depth.valueChanged.connect(self._update_color_response_plot)
        self._update_color_response_plot()

        for combo in (
            self.input_profile,
            self.negative_profile,
            self.print_profile,
            self.output_profile,
            self.film_format,
        ):
            combo.currentIndexChanged.connect(
                self._schedule_metal_preview)

        for control in (
            self.exposure,
            self.negative_flash,
            self.print_flash,
            self.push_pull,
            self.color_density,
            self.color_depth,
            self.negative_bleach_bypass,
            self.print_bleach_bypass,
            self.printer_light_red,
            self.printer_light_green,
            self.printer_light_blue,
            self.printer_light_master,
            self.printer_temperature,
            self.middle_gray,
            self.negative_grain,
            self.print_grain,
            self.grain_size,
            self.grain_chroma,
            self.grain_shadows,
            self.grain_midtones,
            self.grain_highlights,
            self.grain_seed,
            self.image_width_mm,
            self.negative_mtf,
            self.print_mtf,
            self.halation_strength,
            self.halation_radius,
            self.halation_threshold,
        ):
            control.valueChanged.connect(
                self._schedule_metal_preview)

        self.grain_enabled.toggled.connect(self._schedule_metal_preview)
        self.halation_enabled.toggled.connect(self._schedule_metal_preview)
        self.grain_tonal_enabled.toggled.connect(self._schedule_metal_preview)

        image_actions = QWidget()
        image_actions_layout = QHBoxLayout(image_actions)
        image_actions_layout.setContentsMargins(0, 0, 0, 0)
        image_actions_layout.setSpacing(6)
        image_actions_layout.addWidget(self.convert_button, 1)
        image_actions_layout.addWidget(self.open_output_button, 0)
        root_layout.addWidget(image_actions, 0)

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



    def _scope_main_index(self, pane):
        if pane is self.left_scope:
            return 0
        if pane is self.right_scope:
            return 1
        return self.scopes_splitter.count()

    def _restore_scope_to_main(self, pane):
        if pane is None:
            return

        pane.setParent(None)
        pane.pop_out_button.show()
        self.scopes_splitter.insertWidget(
            self._scope_main_index(pane),
            pane)
        pane.show()

        self.scopes_splitter.setStretchFactor(0, 1)
        self.scopes_splitter.setStretchFactor(1, 1)

    @Slot(object)
    def _pop_out_scope(self, pane):
        # Keep a single floating scope window. If one pane is already floating,
        # clicking pop-out on the remaining main pane simply turns on the
        # two-scope split and moves that pane into the existing window.
        if self._floating_scope_window is not None:
            if (
                pane is not self._floating_scope_primary
                and pane is not self._floating_scope_secondary
            ):
                self._floating_scope_window.split_button.setChecked(True)
            self._floating_scope_window.raise_()
            self._floating_scope_window.activateWindow()
            return

        pane.setParent(None)
        pane.show()

        floating = FloatingScopeWindow(pane, self)
        floating.splitChanged.connect(
            self._floating_scope_split_changed)
        floating.closing.connect(
            self._restore_floating_scopes)

        self._floating_scope_window = floating
        self._floating_scope_primary = pane
        self._floating_scope_secondary = None

        floating.show()
        floating.raise_()
        floating.activateWindow()

    @Slot(bool)
    def _floating_scope_split_changed(self, enabled):
        floating = self._floating_scope_window
        if floating is None:
            return

        if enabled:
            if self._floating_scope_secondary is not None:
                return

            other = (
                self.right_scope
                if self._floating_scope_primary is self.left_scope
                else self.left_scope
            )

            # The other scope can only be moved if it is still in the main
            # splitter. This keeps the operation deterministic.
            if other.parent() is not self.scopes_splitter:
                floating.split_button.blockSignals(True)
                floating.split_button.setChecked(False)
                floating.split_button.blockSignals(False)
                return

            other.setParent(None)
            floating.add_scope(other)
            self._floating_scope_secondary = other
            floating.secondary_scope = other
            return

        secondary = floating.remove_secondary()
        if secondary is not None:
            self._floating_scope_secondary = None
            self._restore_scope_to_main(secondary)

    @Slot()
    def _restore_floating_scopes(self):
        floating = self._floating_scope_window
        if floating is None:
            return

        primary = self._floating_scope_primary
        secondary = self._floating_scope_secondary

        # Clear state first so reparenting/close processing cannot re-enter the
        # floating-window logic with stale references.
        self._floating_scope_window = None
        self._floating_scope_primary = None
        self._floating_scope_secondary = None

        if secondary is None:
            secondary = floating.secondary_scope

        if primary is not None:
            self._restore_scope_to_main(primary)
        if secondary is not None and secondary is not primary:
            self._restore_scope_to_main(secondary)

        floating.deleteLater()




    def _response_settings(self):
        settings = {name: control.value() for name, control in self.response_controls.items()}
        if self.response_bypass.isChecked():
            settings["response_amount"] = 0.0
        return settings

    def _update_color_response_plot(self, *_):
        self.response_plot.set_response(filmviz.color_response_preview(
            self._response_settings(), self.color_depth.value(), self.color_density.value()))

    def _color_response_changed(self, *_):
        enabled = not self.response_bypass.isChecked()
        for row in self.response_rows.values():
            row.setEnabled(enabled)
        self.color_depth_control.setEnabled(enabled)
        self._update_color_response_plot()
        self._schedule_metal_preview()

    def _reset_color_response(self):
        self.response_bypass.blockSignals(True)
        self.response_bypass.setChecked(False)
        self.response_bypass.blockSignals(False)
        for name, control in self.response_controls.items():
            control.blockSignals(True)
            control.setValue(self.response_defaults[name])
            control.blockSignals(False)
        self.color_density.setValue(0.0)
        self.color_depth.setValue(1.0)
        self._color_response_changed()

    @Slot()
    def _reset_pipeline(self):
        self._reset_color_response()
        self.input_profile.setCurrentText("ARRI LogC3 (EI800)")
        if self.negative_profile.count() > 0:
            self.negative_profile.setCurrentIndex(0)

        print_index = self.print_profile.findData("kodak-2383")
        if print_index < 0 and self.print_profile.count() > 0:
            print_index = 0
        if print_index >= 0:
            self.print_profile.setCurrentIndex(print_index)

        self.output_profile.setCurrentText("rec709-gamma24")
        self.exposure.setValue(0.0)
        self.negative_flash.setValue(0.0)
        self.print_flash.setValue(0.0)
        self.push_pull.setValue(0.0)
        self.color_density.setValue(0.0)
        self.color_depth.setValue(1.0)
        self.negative_bleach_bypass.setValue(0.0)
        self.print_bleach_bypass.setValue(0.0)
        self.printer_light_red.setValue(25.0)
        self.printer_light_green.setValue(25.0)
        self.printer_light_blue.setValue(25.0)
        self.printer_light_master.setValue(0.0)
        self.printer_temperature.setValue(3200.0)
        self.middle_gray.setValue(0.18)
        self.lut_size.setValue(65)
        self.use_lut_acceleration.setChecked(True)
        self.threads.setValue(0)
        self._print_profile_changed()

    @Slot()
    def _reset_image_settings(self):
        self.grain_enabled.setChecked(True)
        self.halation_enabled.setChecked(True)
        self.negative_grain.setValue(0.0)
        self.print_grain.setValue(0.0)
        self.grain_size.setValue(1.0)
        self.grain_chroma.setValue(1.0)
        self.grain_tonal_enabled.setChecked(True)
        self.grain_shadows.setValue(1.0)
        self.grain_midtones.setValue(1.0)
        self.grain_highlights.setValue(1.0)
        self.grain_seed.setValue(1)

        format_index = self.film_format.findData("super-35")
        if format_index >= 0:
            self.film_format.setCurrentIndex(format_index)

        self.negative_mtf.setValue(0.0)
        self.print_mtf.setValue(0.0)
        self.halation_strength.setValue(0.0)
        self.halation_radius.setValue(12.0)
        self.halation_threshold.setValue(0.7)
        self._film_format_changed()

    @Slot()
    def _reset_lut_settings(self):
        self.output_lut.edit.setText(
            str(PROJECT_ROOT / "build" / "filmviz.cube"))

    @Slot()
    def _reset_profiles(self):
        self._discard_runtime_resources()
        self._reload_profile_plot()

    @Slot()
    def _print_profile_changed(self):
        negative_only = self.print_profile.currentData() == "none"
        for widget in (
            self.print_flash_control,
            self.print_bleach_bypass_control,
            self.printer_light_master_control,
            self.printer_light_red_control,
            self.printer_light_green_control,
            self.printer_light_blue_control,
            self.printer_temperature,
            self.print_grain_control,
            self.print_mtf_control,
        ):
            widget.setEnabled(not negative_only)

    @Slot()
    def _film_format_changed(self):
        identifier = self.film_format.currentData()
        format_entry = self.film_formats_by_id.get(identifier)

        if format_entry is not None and identifier != "custom":
            self.image_width_mm.setValue(
                float(format_entry["image_width_mm"]))

        self.image_width_mm.setEnabled(identifier == "custom")

    def _load_reference_image(self, filename: str):
        if not self.image_preview.has_image():
            return

        try:
            preview = filmviz.read_image_preview(filename, 1600)
            width = int(preview["width"])
            height = int(preview["height"])
            rgb = bytes(preview["rgb"])
        except Exception as error:
            QMessageBox.warning(
                self,
                "Could not load reference image",
                str(error))
            return

        self.image_preview.set_reference_rgb(
            width,
            height,
            rgb)
        self._reference_image_filename = str(Path(filename).expanduser().resolve())

        self.reference_wipe.setEnabled(True)
        self.clear_reference_button.setEnabled(True)
        self.reference_icon.setToolTip(
            f"Reference: {Path(filename).name}\n"
            "Drop another image onto the preview to replace it.")

        try:
            scope_width, scope_height, scope_rgb = _read_scope_rgb(filename)
        except Exception as error:
            self._reference_scope_width = 0
            self._reference_scope_height = 0
            self._reference_scope_rgb = None
            self.left_scope.clear_reference()
            self.right_scope.clear_reference()
            QMessageBox.warning(
                self,
                "Reference scopes unavailable",
                str(error))
            return

        self._reference_scope_width = scope_width
        self._reference_scope_height = scope_height
        self._reference_scope_rgb = scope_rgb

        self.left_scope.set_reference_rgb(
            scope_width,
            scope_height,
            scope_rgb)
        self.right_scope.set_reference_rgb(
            scope_width,
            scope_height,
            scope_rgb)
        self.left_scope.set_reference_wipe(
            self.reference_wipe.value())
        self.right_scope.set_reference_wipe(
            self.reference_wipe.value())
        self._resample_probes()

    @Slot()
    def _clear_reference_image(self):
        self.image_preview.clear_reference()
        self.left_scope.clear_reference()
        self.right_scope.clear_reference()
        self._reference_scope_width = 0
        self._reference_scope_height = 0
        self._reference_scope_rgb = None
        self._reference_image_filename = None
        self.reference_wipe.setEnabled(False)
        self.clear_reference_button.setEnabled(False)
        self.reference_icon.setToolTip(
            "Drop a reference image onto the converted-image preview.")
        self._resample_probes()

    def _load_diagnostics(self, filename: str):
        try:
            preview = filmviz.read_image_preview(filename, 1600)
            preview_width = int(preview["width"])
            preview_height = int(preview["height"])
            preview_rgb = bytes(preview["rgb"])
        except Exception as error:
            self.image_preview.set_error(
                f"Could not load converted image:\n{error}")
            return

        try:
            scope_width, scope_height, scope_rgb = _read_scope_rgb(filename)
        except Exception as error:
            self.image_preview.set_rgb(
                preview_width,
                preview_height,
                preview_rgb,
                image_identity=str(Path(filename).expanduser().resolve()))
            self._scope_width = 0
            self._scope_height = 0
            self._scope_rgb = None
            self._resample_probes()
            QMessageBox.warning(
                self,
                "High-precision scopes unavailable",
                str(error))
            return

        self.image_preview.set_rgb(
            preview_width,
            preview_height,
            preview_rgb,
            image_identity=str(Path(filename).expanduser().resolve()))

        self._scope_width = scope_width
        self._scope_height = scope_height
        self._scope_rgb = scope_rgb

        self.left_scope.set_rgb(
            scope_width,
            scope_height,
            scope_rgb)
        self.right_scope.set_rgb(
            scope_width,
            scope_height,
            scope_rgb)
        self._resample_probes()


    def _current_probe_source_identity(self):
        value = self.input_image.value()
        if not value:
            return None
        try:
            return str(Path(value).expanduser().resolve())
        except Exception:
            return value

    def _evaluate_probe(self, u: float, v: float):
        arguments = self._common()
        return filmviz.probe_image_pixel(
            resources=arguments["resources"],
            input_filename=self.input_image.value(),
            input=arguments["input"],
            negative=arguments["negative"],
            print=arguments["print"],
            exposure=arguments["exposure"],
            negative_flash=arguments["negative_flash"],
            print_flash=arguments["print_flash"],
            push_pull=arguments["push_pull"],
            color_density=arguments["color_density"],
            color_depth=arguments["color_depth"],
            color_response=arguments["color_response"],
            negative_bleach_bypass=arguments["negative_bleach_bypass"],
            print_bleach_bypass=arguments["print_bleach_bypass"],
            printer_light_red=arguments["printer_light_red"],
            printer_light_green=arguments["printer_light_green"],
            printer_light_blue=arguments["printer_light_blue"],
            printer_light_master=arguments["printer_light_master"],
            middle_gray=arguments["middle_gray"],
            printer_temperature=arguments["printer_temperature"],
            u=u,
            v=v,
        )

    def _format_probe_result(self, index, entry):
        probe = entry["probe"]
        reference_rgb = entry.get("reference_rgb")

        def triplet(name):
            values = probe[name]
            return (
                f"({float(values[0]):.6g}, "
                f"{float(values[1]):.6g}, "
                f"{float(values[2]):.6g})"
            )

        def field(label, value):
            return f"{label:<18} {value}"

        left = [
            field("encoded RGB", triplet("encoded_rgb")),
            field("input AP0", triplet("ap0_input")),
            field("negative H R/G/B", triplet("negative_exposure")),
            field("Status-M D R/G/B", triplet("negative_status_m")),
            field("calibrated D", triplet("negative_calibrated")),
            "",
        ]
        right = [
            field("color response D", triplet("negative_color_response")),
            field("print H R/G/B", triplet("print_exposure")),
            field("print D R/G/B", triplet("print_density")),
            field("output AP0", triplet("output_ap0")),
            field("Rec709 linear raw", triplet("output_rec709_linear_unclamped")),
            field("output Rec709", triplet("output_rec709_gamma24")),
        ]

        column_width = max(len(line) for line in left) + 5
        rows = [
            left_line.ljust(column_width) + right_line
            for left_line, right_line in zip(left, right)
        ]

        spectrum = probe["spectrum"]
        spectral_text = " ".join(
            f"{w}:{float(spectrum[w]):.4g}"
            for w in (
                "400", "420", "440", "460", "500", "550",
                "600", "620", "640", "660", "680"
            )
        )

        reference_lines = []
        if reference_rgb is not None:
            reference_text = (
                f"({reference_rgb[0]:.7g}, "
                f"{reference_rgb[1]:.7g}, "
                f"{reference_rgb[2]:.7g})")
            output_rgb = probe["output_rec709_gamma24"]
            delta = tuple(
                float(reference_rgb[channel]) - float(output_rgb[channel])
                for channel in range(3))
            delta_text = (
                f"({delta[0]:+.7g}, {delta[1]:+.7g}, {delta[2]:+.7g})")
            reference_lines.extend((
                f"reference RGB     {reference_text}",
                f"ref - output RGB  {delta_text}",
            ))

        return (
            f"Pick {index} — pixel ({probe['x']}, {probe['y']}) / "
            f"{probe['width']}×{probe['height']}\n"
            + "\n".join(rows)
            + f"\nspectrum          {spectral_text}"
            + (("\n" + "\n".join(reference_lines)) if reference_lines else "")
        )

    def _refresh_probe_output(self):
        if not self._probe_results:
            self.probe_output.clear()
            return

        self.probe_output.setPlainText(
            "\n\n".join(
                self._format_probe_result(index, entry)
                for index, entry in enumerate(self._probe_results, start=1)
            )
        )

    def _refresh_probe_markers(self):
        self.image_preview.set_probes(self._probe_points)

        scope_probes = []
        for u, v in self._probe_points:
            scope_rgb = _scope_rgb_at(
                self._scope_width,
                self._scope_height,
                self._scope_rgb,
                u,
                v)
            if scope_rgb is not None:
                scope_probes.append((u, v, scope_rgb))

        self.left_scope.set_probes(scope_probes)
        self.right_scope.set_probes(scope_probes)

    @Slot()
    def _clear_probes(self):
        self._probe_points = []
        self._probe_results = []
        self._probe_source_identity = None
        self.image_preview.set_probes([])
        self.left_scope.clear_probe()
        self.right_scope.clear_probe()
        self.probe_output.clear()

    def _resample_probes(self):
        if not self._probe_points:
            self._refresh_probe_markers()
            self._refresh_probe_output()
            return

        current_identity = self._current_probe_source_identity()
        if (
            self._probe_source_identity is not None
            and current_identity != self._probe_source_identity
        ):
            self._clear_probes()
            return

        self._probe_source_identity = current_identity
        results = []
        for u, v in self._probe_points:
            try:
                probe = self._evaluate_probe(u, v)
            except Exception as error:
                self.probe_output.setPlainText(
                    f"Probe resampling failed:\n{error}")
                return
            reference_rgb = _scope_rgb_at(
                self._reference_scope_width,
                self._reference_scope_height,
                self._reference_scope_rgb,
                u, v)
            results.append({
                "u": u,
                "v": v,
                "probe": probe,
                "reference_rgb": reference_rgb,
            })

        self._probe_results = results
        self._refresh_probe_markers()
        self._refresh_probe_output()

    @Slot(float, float)
    def _probe_image_pixel(self, u: float, v: float):
        current_identity = self._current_probe_source_identity()
        if (
            self._probe_source_identity is not None
            and current_identity != self._probe_source_identity
        ):
            self._clear_probes()

        try:
            probe = self._evaluate_probe(u, v)
        except Exception as error:
            self.probe_output.setPlainText(
                f"Probe failed:\n{error}")
            return

        self._probe_source_identity = current_identity
        self._probe_points.append((float(u), float(v)))
        reference_rgb = _scope_rgb_at(
            self._reference_scope_width,
            self._reference_scope_height,
            self._reference_scope_rgb,
            u, v)
        self._probe_results.append({
            "u": float(u),
            "v": float(v),
            "probe": probe,
            "reference_rgb": reference_rgb,
        })
        self._refresh_probe_markers()
        self._refresh_probe_output()

    def _agent_snapshot_text(self):
        common = self._common()

        def value(widget):
            if isinstance(widget, QDoubleSpinBox):
                return f"{widget.value():g}"
            if isinstance(widget, QSpinBox):
                return str(widget.value())
            if isinstance(widget, QCheckBox):
                return "On" if widget.isChecked() else "Off"
            return str(widget)

        lines = [
            "FilmViz agent snapshot",
            "",
            "[Files]",
            f"Input image: {self.input_image.value()}",
            f"Converted image: {self._last_output_image or self.output_image.value()}",
            f"Reference image: {self._reference_image_filename or 'None'}",
            f"Reference wipe: {self.reference_wipe.value()}%",
            f"Display: Rec.709 Gamma 2.4",
            "",
            "[Pipeline]",
            f"Resource directory: {common['resources']}",
            f"Input profile: {common['input']}",
            f"Negative: {common['negative']}",
            f"Print: {common['print']}",
            f"Output profile: {common['output']}",
            f"Exposure stops: {common['exposure']:g}",
            f"Negative flash (%): {common['negative_flash']:g}",
            f"Print flash (%): {common['print_flash']:g}",
            f"Push/pull stops: {common['push_pull']:g}",
            f"Color separation: {common['color_density']:g}",
            f"Color depth: {common['color_depth']:g}",
            f"Negative bypass: {common['negative_bleach_bypass']:g}",
            f"Print bypass: {common['print_bleach_bypass']:g}",
            f"Printer R light: {common['printer_light_red']:g}",
            f"Printer G light: {common['printer_light_green']:g}",
            f"Printer B light: {common['printer_light_blue']:g}",
            f"Printer master: {common['printer_light_master']:g}",
            f"Printer K: {common['printer_temperature']:g}",
            f"Middle gray: {common['middle_gray']:g}",
            f"LUT size: {common['lut_size']}",
            f"Use LUT acceleration: {'On' if common['use_lut_acceleration'] else 'Off'}",
            f"Worker threads: {common['threads']}",
            "",
            "[Image]",
            f"Negative grain: {self.negative_grain.value():g}",
            f"Print grain: {self.print_grain.value():g}",
            f"Grain scale (×): {self.grain_size.value():g}",
            f"Grain enabled: {self.grain_enabled.isChecked()}",
            f"Halation enabled: {self.halation_enabled.isChecked()}",
            f"Grain chroma: {self.grain_chroma.value():g}",
            f"Tonal grain shaping: {self.grain_tonal_enabled.isChecked()}",
            f"Grain shadows/midtones/highlights: {self.grain_shadows.value():g} / "
            f"{self.grain_midtones.value():g} / {self.grain_highlights.value():g}",
            f"Grain seed: {self.grain_seed.value()}",
            f"Film format: {self.film_format.currentText()}",
            f"Active image width (mm): {self.image_width_mm.value():g}",
            f"Negative MTF: {self.negative_mtf.value():g}%",
            f"Print MTF: {self.print_mtf.value():g}%",
            f"Halation: {self.halation_strength.value():g}",
            f"Halation radius (px): {self.halation_radius.value():g}",
            f"Halation threshold: {self.halation_threshold.value():g}",
        ]

        probe_text = self.probe_output.toPlainText().strip()
        if probe_text:
            lines.extend(("", "[Picked pixels]", probe_text))

        return "\n".join(lines)

    def _render_scope_image(
        self,
        result_widget,
        reference_widget,
        width: int,
        height: int,
    ):
        def render(widget):
            old_size = QSize(widget.size())
            widget.resize(width, height)
            image = QImage(
                width,
                height,
                QImage.Format.Format_ARGB32_Premultiplied)
            image.fill(QColor("#0b0d0f"))
            painter = QPainter(image)
            try:
                # PySide6 requires the target offset when rendering through
                # an existing QPainter. Passing only the painter matches no
                # overload and can leave the paint device active on failure.
                widget.render(painter, QPoint(0, 0))
            finally:
                painter.end()
                widget.resize(old_size)
            return image

        result = render(result_widget)
        if not self.image_preview.has_reference():
            return result

        reference = render(reference_widget)
        wipe_x = int(round(width * self.reference_wipe.value() / 100.0))
        wipe_x = min(width, max(0, wipe_x))

        composed = QImage(result)
        painter = QPainter(composed)
        if wipe_x > 0:
            painter.save()
            painter.setClipRect(QRect(0, 0, wipe_x, height))
            painter.drawImage(QRect(0, 0, width, height), reference)
            painter.restore()
        painter.setPen(QPen(QColor(245, 210, 70), 1.0))
        painter.drawLine(wipe_x, 0, wipe_x, max(0, height - 1))
        painter.end()
        return composed

    def _agent_scope_images(self, width: int, height: int):
        pane = self.left_scope

        vector_zoom = pane.vector_scope._zoom2
        reference_vector_zoom = pane.reference_vector_scope._zoom2
        pane.vector_scope.set_zoom2(False)
        pane.reference_vector_scope.set_zoom2(False)

        waveform_mode = pane.waveform._mode
        reference_waveform_mode = pane.reference_waveform._mode

        images = []
        try:
            images.append((
                "Vectorscope",
                self._render_scope_image(
                    pane.vector_scope,
                    pane.reference_vector_scope,
                    width,
                    height)))
            images.append((
                "RGB Histogram",
                self._render_scope_image(
                    pane.histogram,
                    pane.reference_histogram,
                    width,
                    height)))
            images.append((
                "RGB Parade",
                self._render_scope_image(
                    pane.parade,
                    pane.reference_parade,
                    width,
                    height)))

            pane.waveform.set_mode("rgb")
            pane.reference_waveform.set_mode("rgb")
            images.append((
                "RGB Waveform",
                self._render_scope_image(
                    pane.waveform,
                    pane.reference_waveform,
                    width,
                    height)))

            pane.waveform.set_mode("y")
            pane.reference_waveform.set_mode("y")
            images.append((
                "Y Waveform",
                self._render_scope_image(
                    pane.waveform,
                    pane.reference_waveform,
                    width,
                    height)))
        finally:
            pane.vector_scope.set_zoom2(vector_zoom)
            pane.reference_vector_scope.set_zoom2(reference_vector_zoom)
            pane.waveform.set_mode(waveform_mode)
            pane.reference_waveform.set_mode(reference_waveform_mode)

        return images

    def _build_agent_snapshot(self):
        if not self.image_preview.has_image():
            return QImage()

        source = self.image_preview._image
        reference = self.image_preview._reference_image

        page_width = 1600
        margin = 28
        gap = 22
        settings_width = 520
        image_width = page_width - margin * 2 - gap - settings_width
        max_image_height = 690
        image_height = min(
            max_image_height,
            max(1, int(round(image_width * source.height() / max(1, source.width())))))

        scope_width = (page_width - margin * 2 - gap) // 2
        scope_height = 330
        scope_title_height = 28
        scope_gap = 18
        scope_images = self._agent_scope_images(scope_width, scope_height)
        scope_rows = (len(scope_images) + 1) // 2
        scopes_height = scope_rows * (scope_title_height + scope_height) + max(0, scope_rows - 1) * scope_gap

        top_height = max(image_height, 690)
        page_height = margin + top_height + 34 + scopes_height + margin

        page = QImage(
            page_width,
            page_height,
            QImage.Format.Format_ARGB32_Premultiplied)
        page.fill(QColor("#101214"))

        painter = QPainter(page)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)

        image_rect = QRectF(margin, margin, image_width, image_height)
        painter.fillRect(image_rect, QColor("#07090a"))
        painter.drawImage(image_rect, source, QRectF(source.rect()))

        if not reference.isNull():
            wipe_x = image_rect.left() + image_rect.width() * self.reference_wipe.value() / 100.0
            painter.save()
            painter.setClipRect(QRectF(
                image_rect.left(),
                image_rect.top(),
                max(0.0, wipe_x - image_rect.left()),
                image_rect.height()))
            painter.drawImage(image_rect, reference, QRectF(reference.rect()))
            painter.restore()
            painter.setPen(QPen(QColor(245, 210, 70), 1.5))
            painter.drawLine(
                QPointF(wipe_x, image_rect.top()),
                QPointF(wipe_x, image_rect.bottom()))

        text_left = margin + image_width + gap
        text_rect = QRectF(text_left, margin, settings_width, top_height)
        painter.fillRect(text_rect, QColor("#17191b"))

        title_font = self.font()
        title_font.setBold(True)
        title_font.setPixelSize(21)
        painter.setFont(title_font)
        painter.setPen(QColor(232, 232, 232))
        painter.drawText(
            QRectF(text_left + 18, margin + 16, settings_width - 36, 30),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            "FilmViz settings")

        body_font = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        body_font.setPixelSize(13)
        painter.setFont(body_font)
        painter.setPen(QColor(205, 205, 205))
        painter.drawText(
            QRectF(text_left + 18, margin + 52, settings_width - 36, top_height - 68),
            Qt.AlignmentFlag.AlignLeft
            | Qt.AlignmentFlag.AlignTop
            | Qt.TextFlag.TextWordWrap,
            self._agent_snapshot_text())

        y = margin + top_height + 34
        for index, (title, scope_image) in enumerate(scope_images):
            row = index // 2
            column = index % 2
            x = margin + column * (scope_width + gap)
            sy = y + row * (scope_title_height + scope_height + scope_gap)

            painter.setFont(title_font)
            painter.setPen(QColor(225, 225, 225))
            painter.drawText(
                QRectF(x, sy, scope_width, scope_title_height),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                title)
            painter.drawImage(
                QRectF(x, sy + scope_title_height, scope_width, scope_height),
                scope_image)

        painter.end()
        page.setColorSpace(APP_COLOR_SPACE)
        return page

    @Slot()
    def _copy_agent_snapshot(self):
        if not self.image_preview.has_image():
            return

        snapshot = self._build_agent_snapshot()
        if snapshot.isNull():
            return

        mime = QMimeData()
        mime.setImageData(snapshot)
        mime.setText(self._agent_snapshot_text())
        QApplication.clipboard().setMimeData(mime)

    @Slot()
    def _copy_agent_text(self):
        QApplication.clipboard().setText(self._agent_snapshot_text())

    @Slot()
    def _copy_converted_image(self):
        if not self.image_preview.has_image():
            return
        QApplication.clipboard().setImage(self.image_preview._image)

    @Slot()
    def _copy_converted_image_with_markers(self):
        if not self.image_preview.has_image():
            return

        image = self.image_preview.converted_image_with_markers()
        if image.isNull():
            return
        QApplication.clipboard().setImage(image)

    def _discard_runtime_resources(self):
        runtime = self._runtime_resources
        self._runtime_resources = None
        self._runtime_resources_source = None
        self._profile_edit_path = None
        self._profile_edit_x_column = None
        if runtime is not None:
            try:
                runtime.cleanup()
            except Exception:
                pass
        if hasattr(self, "profile_reset_profile_button"):
            self.profile_reset_profile_button.setEnabled(False)
        if hasattr(self, "profile_reset_curve_button"):
            self.profile_reset_curve_button.setEnabled(False)
        if hasattr(self, "profile_save_button"):
            self.profile_save_button.setEnabled(False)
        if hasattr(self, "realtime_metal"):
            self._schedule_profile_metal_preview()

    @Slot()
    def _resources_changed(self):
        try:
            profiles = filmviz.profiles(str(Path(self.resources.value()).expanduser()))
        except Exception as error:
            QMessageBox.warning(self, "OCIO configuration", str(error))
            return
        self.input_profile.setProfiles(profiles["input_details"])
        self.output_profile.setProfiles(profiles["output_details"])
        source = str(Path(self.resources.value()).expanduser().resolve())
        if (
            self._runtime_resources is not None
            and self._runtime_resources_source != source
        ):
            self._discard_runtime_resources()
        self._reload_profile_plot()
        self._schedule_profile_metal_preview()

    def _effective_resources_path(self):
        if self._runtime_resources is not None:
            return Path(self._runtime_resources.name)
        return Path(self.resources.value()).expanduser()

    def _ensure_runtime_resources(self):
        source = Path(self.resources.value()).expanduser().resolve()
        if self._runtime_resources is not None:
            if self._runtime_resources_source == str(source):
                return Path(self._runtime_resources.name)
            self._discard_runtime_resources()

        if not source.is_dir():
            raise RuntimeError(f"Resource directory does not exist: {source}")

        runtime = tempfile.TemporaryDirectory(prefix="filmviz_profile_edit_")
        target = Path(runtime.name)
        shutil.copytree(source, target, dirs_exist_ok=True)
        self._runtime_resources = runtime
        self._runtime_resources_source = str(source)
        return target

    def _current_profile_info(self):
        data = self.profile_family.currentData()
        if not data:
            return None
        family, identifier = data
        if family == "negative":
            profile = self.negative_profiles_by_id.get(identifier)
        else:
            profile = self.print_profiles_by_id.get(identifier)
        if profile is None:
            return None
        return family, identifier, profile

    def _current_profile_paths(self):
        info = self._current_profile_info()
        filename = self.profile_curve_type.currentData()
        if info is None or not filename:
            return None, None, None

        family, identifier, profile = info
        relative = Path(profile["resource_directory"]) / filename
        source = Path(self.resources.value()).expanduser() / relative
        effective = self._effective_resources_path() / relative
        return source, effective, relative

    @staticmethod
    def _read_csv_table(path: Path):
        with path.open("r", encoding="utf-8-sig", errors="replace", newline="") as handle:
            reader = csv.reader(handle)
            rows = list(reader)
        if not rows:
            raise RuntimeError(f"CSV is empty: {path}")
        return rows[0], rows[1:]

    @staticmethod
    def _write_csv_table(path: Path, header, rows):
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        with temporary.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle, lineterminator="\n")
            writer.writerow(header)
            writer.writerows(rows)
        temporary.replace(path)

    def _write_profile_point(self, curve_name: str, point_index: int, value: float):
        source_path, effective_path, relative = self._current_profile_paths()
        if source_path is None or self._profile_edit_x_column is None:
            return

        runtime_root = self._ensure_runtime_resources()
        path = runtime_root / relative
        header, rows = self._read_csv_table(path)
        if curve_name not in header or self._profile_edit_x_column not in header:
            raise RuntimeError(
                f"Curve columns are unavailable in {path.name}: "
                f"{self._profile_edit_x_column}, {curve_name}")

        x_index = header.index(self._profile_edit_x_column)
        y_index = header.index(curve_name)
        valid_row_indices = []
        for row_index, row in enumerate(rows):
            if x_index >= len(row) or y_index >= len(row):
                continue
            try:
                float(row[x_index])
                float(row[y_index])
            except (TypeError, ValueError):
                continue
            valid_row_indices.append(row_index)

        if point_index < 0 or point_index >= len(valid_row_indices):
            raise RuntimeError(
                f"Point {point_index} is outside curve {curve_name} in {path.name}")

        row_index = valid_row_indices[point_index]
        while len(rows[row_index]) < len(header):
            rows[row_index].append("")
        rows[row_index][y_index] = format(float(value), ".12g")
        self._write_csv_table(path, header, rows)
        self._profile_edit_path = path
        self.profile_reset_curve_button.setEnabled(True)
        self.profile_reset_profile_button.setEnabled(True)
        self.profile_save_button.setEnabled(True)

    @Slot()
    def _profile_edit_spacing_changed(self):
        spacing = float(self.profile_edit_spacing.currentData() or 0.0)
        self.profile_plot.set_edit_spacing_nm(spacing)
        self.profile_selected_curve.setText("No point selected")
        self.profile_point_x.setEnabled(False)
        self.profile_point_y.setEnabled(False)

    @Slot(int, int, str, float, float)
    def _profile_point_selected(self, curve_index, point_index, name, x, y):
        self.profile_selected_curve.setText(name)
        self.profile_point_x.blockSignals(True)
        self.profile_point_y.blockSignals(True)
        self.profile_point_x.setValue(float(x))
        self.profile_point_y.setValue(float(y))
        self.profile_point_x.setEnabled(True)
        self.profile_point_y.setEnabled(True)
        self.profile_point_x.blockSignals(False)
        self.profile_point_y.blockSignals(False)
        self.profile_reset_curve_button.setEnabled(
            self._runtime_resources is not None)
        self.profile_reset_profile_button.setEnabled(
            self._runtime_resources is not None)

    def _write_profile_curve(self, curve_name: str, points):
        source_path, effective_path, relative = self._current_profile_paths()
        if source_path is None or self._profile_edit_x_column is None:
            return

        runtime_root = self._ensure_runtime_resources()
        path = runtime_root / relative
        header, rows = self._read_csv_table(path)
        if curve_name not in header or self._profile_edit_x_column not in header:
            raise RuntimeError(
                f"Curve columns are unavailable in {path.name}: "
                f"{self._profile_edit_x_column}, {curve_name}")

        x_index = header.index(self._profile_edit_x_column)
        y_index = header.index(curve_name)
        point_by_x = {round(float(x), 9): float(y) for x, y in points}
        for row in rows:
            if x_index >= len(row):
                continue
            try:
                x = float(row[x_index])
            except (TypeError, ValueError):
                continue
            key = round(x, 9)
            if key not in point_by_x:
                continue
            while len(row) < len(header):
                row.append("")
            row[y_index] = format(point_by_x[key], ".12g")

        self._write_csv_table(path, header, rows)
        self._profile_edit_path = path
        self.profile_reset_curve_button.setEnabled(True)
        self.profile_reset_profile_button.setEnabled(True)
        self.profile_save_button.setEnabled(True)

    @Slot(int, int, str, float, float)
    def _profile_point_changed(self, curve_index, point_index, name, x, y):
        try:
            self._write_profile_curve(
                name, self.profile_plot.curve_points(curve_index))
        except Exception as error:
            QMessageBox.warning(self, "Could not edit profile curve", str(error))
            self._reload_profile_plot()
            return

        self.profile_point_y.blockSignals(True)
        self.profile_point_y.setValue(float(y))
        self.profile_point_y.blockSignals(False)
        self._schedule_profile_metal_preview()

    @Slot(float)
    def _profile_numeric_y_changed(self, value: float):
        if not self.profile_point_y.isEnabled():
            return
        self.profile_plot.set_selected_y(float(value))

    @Slot()
    def _reset_current_profile_curve(self):
        selected = self.profile_plot.selected_point()
        paths = self._current_profile_paths()
        if selected is None or self._runtime_resources is None:
            return

        source_path, effective_path, relative = paths
        curve_name = selected[2]
        source_header, source_rows = self._read_csv_table(source_path)
        runtime_header, runtime_rows = self._read_csv_table(effective_path)
        if curve_name not in source_header or curve_name not in runtime_header:
            return

        source_index = source_header.index(curve_name)
        runtime_index = runtime_header.index(curve_name)
        for row_index in range(min(len(source_rows), len(runtime_rows))):
            if source_index >= len(source_rows[row_index]):
                continue
            while len(runtime_rows[row_index]) < len(runtime_header):
                runtime_rows[row_index].append("")
            runtime_rows[row_index][runtime_index] = source_rows[row_index][source_index]

        self._write_csv_table(effective_path, runtime_header, runtime_rows)
        self._reload_profile_plot()
        self._schedule_profile_metal_preview()

    @Slot()
    def _reset_current_profile(self):
        if self._runtime_resources is None:
            return
        info = self._current_profile_info()
        if info is None:
            return
        family, identifier, profile = info
        relative_directory = Path(profile["resource_directory"])
        source_directory = Path(self.resources.value()).expanduser() / relative_directory
        runtime_directory = Path(self._runtime_resources.name) / relative_directory
        if runtime_directory.exists():
            shutil.rmtree(runtime_directory)
        shutil.copytree(source_directory, runtime_directory)
        self._reload_profile_plot()
        self._schedule_profile_metal_preview()

    @Slot()
    def _save_current_profile(self):
        if self._runtime_resources is None:
            QMessageBox.information(
                self,
                "No runtime profile edits",
                "Change at least one curve point before saving an edited profile.")
            return

        info = self._current_profile_info()
        if info is None:
            return

        family, identifier, profile = info
        relative_directory = Path(profile["resource_directory"])
        runtime_directory = Path(self._runtime_resources.name) / relative_directory
        if not runtime_directory.is_dir():
            QMessageBox.warning(
                self,
                "Could not save profile",
                f"Runtime profile directory is unavailable:\n{runtime_directory}")
            return

        suggested_root = Path(self.resources.value()).expanduser().parent
        destination_root = QFileDialog.getExistingDirectory(
            self,
            "Choose folder for edited FilmViz profile",
            str(suggested_root))
        if not destination_root:
            return

        safe_identifier = str(identifier).replace("/", "_").replace("\\", "_")
        destination = Path(destination_root) / f"{safe_identifier}_edited"

        if destination.exists():
            answer = QMessageBox.question(
                self,
                "Replace edited profile?",
                f"The destination already exists:\n{destination}\n\nReplace it?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes:
                return
            shutil.rmtree(destination)

        try:
            shutil.copytree(runtime_directory, destination)
        except Exception as error:
            QMessageBox.critical(
                self,
                "Could not save profile",
                str(error))
            return

        QMessageBox.information(
            self,
            "Edited profile saved",
            "Saved the complete edited profile directory.\n\n"
            f"{destination}\n\n"
            "The canonical resource CSV files were not changed.")

    @Slot()
    def _profile_family_changed(self):
        current_label = self.profile_curve_type.currentText()
        self.profile_curve_type.blockSignals(True)
        self.profile_curve_type.clear()

        family, identifier = self.profile_family.currentData()

        if family == "negative":
            profile = self.negative_profiles_by_id[identifier]
            prefix = profile["resource_prefix"]
            entries = [
                ("Spectral sensitivity", f"{prefix}_spectral_sensitivity_curves.csv"),
                ("Sensitometric curves", f"{prefix}_sensitometric_curves.csv"),
                ("Spectral dye density", f"{prefix}_spectral_dye_density_curves.csv"),
                ("Diffuse RMS granularity", f"{prefix}_diffuse_rms_granularity_curves.csv"),
            ]

            mtf_filename = f"{prefix}_modulation_transfer_function_curves.csv"
            mtf_path = (
                Path(self.resources.value())
                / profile["resource_directory"]
                / mtf_filename
            )
            if mtf_path.exists():
                entries.insert(3, ("MTF", mtf_filename))
        else:
            profile = self.print_profiles_by_id[identifier]
            entries = (
                ("Spectral sensitivity", profile["sensitivity_filename"]),
                ("Sensitometric curves", profile["characteristic_filename"]),
                ("Spectral dye density", profile["dye_density_filename"]),
                ("MTF", profile["mtf_filename"]),
                ("Diffuse RMS granularity", profile["granularity_filename"]),
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
        self.profile_selected_curve.setText("No point selected")
        self.profile_point_x.setEnabled(False)
        self.profile_point_y.setEnabled(False)
        self._profile_edit_x_column = None

        if not filename:
            self.profile_plot.set_error("No profile curve selected.")
            return

        family, identifier = self.profile_family.currentData()
        if family == "negative":
            profile_directory = self.negative_profiles_by_id[
                identifier]["resource_directory"]
        else:
            profile_directory = self.print_profiles_by_id[
                identifier]["resource_directory"]

        source_path = (
            Path(self.resources.value()).expanduser()
            / profile_directory
            / filename
        )
        path = (
            self._effective_resources_path()
            / profile_directory
            / filename
        )

        x_column = None
        y_columns = None
        stop_axis = None

        if filename.endswith("_spectral_dye_density_curves.csv"):
            x_column = "wavelength_nm"

            if family == "negative":
                y_columns = (
                    "minimum_density",
                    "midscale_neutral_density",
                    "cyan_peak_normalized",
                    "magenta_peak_normalized",
                    "yellow_peak_normalized",
                )
            else:
                y_columns = (
                    "visual_neutral_density",
                    "cyan_density",
                    "magenta_density",
                    "yellow_density",
                )

        elif filename.endswith("_sensitometric_curves.csv"):
            if family == "negative":
                x_column = "log_exposure_lux_seconds"
                y_columns = (
                    "curve_high_density",
                    "curve_mid_density",
                    "curve_low_density",
                )
                stop_axis = (-8.0, 8.0, -0.515)

        x_label, curves = _read_curve_csv(
            path,
            x_column=x_column,
            y_columns=y_columns)
        original_x_label, original_curves = _read_curve_csv(
            source_path,
            x_column=x_column,
            y_columns=y_columns)

        if not curves:
            self.profile_plot.set_error(
                f"Could not load curve data:\n{path}")
            return

        self._profile_edit_path = path
        self._profile_edit_x_column = x_label

        title = (
            f"{self.profile_family.currentText()} — "
            f"{self.profile_curve_type.currentText()}"
        )
        if self._runtime_resources is not None and path != source_path:
            title += "  [runtime edited]"

        self.profile_plot.set_curves(
            title,
            x_label,
            curves,
            stop_axis=stop_axis,
            original_curves=original_curves or curves)

        # Edit spacing follows the actual X axis. Every curve remains editable;
        # only the coarser anchor choices change with the axis units.
        previous_spacing = float(self.profile_edit_spacing.currentData() or 0.0)
        self.profile_edit_spacing.blockSignals(True)
        self.profile_edit_spacing.clear()
        self.profile_edit_spacing.addItem("All samples", 0.0)

        if x_label == "wavelength_nm":
            for label, spacing in ((
                ("5 nm", 5.0),
                ("10 nm", 10.0),
                ("20 nm", 20.0),
                ("25 nm", 25.0),
                ("50 nm", 50.0),
            )):
                self.profile_edit_spacing.addItem(label, spacing)
            self.profile_edit_spacing.setToolTip(
                "Choose editable wavelength anchors. Samples between anchors "
                "are linearly interpolated in wavelength.")

        elif x_label == "log_exposure_lux_seconds":
            # One photographic stop is log10(2) in LogE. Present the editor
            # in camera-stop units while keeping the native LogE X values.
            log10_two = 0.3010299956639812
            for label, stops in ((
                ("1/4 stop", 0.25),
                ("1/2 stop", 0.5),
                ("1 stop", 1.0),
                ("2 stops", 2.0),
            )):
                self.profile_edit_spacing.addItem(label, stops * log10_two)
            self.profile_edit_spacing.setToolTip(
                "Choose editable exposure anchors. Samples between anchors "
                "are linearly interpolated in LogE.")

        else:
            # For other numeric X axes, expose useful multiples of the native
            # sample spacing rather than assuming wavelength units.
            all_x = sorted({
                float(x)
                for _, points in curves
                for x, _ in points
            })
            deltas = [
                all_x[index] - all_x[index - 1]
                for index in range(1, len(all_x))
                if all_x[index] > all_x[index - 1]
            ]
            if deltas:
                deltas.sort()
                native = deltas[len(deltas) // 2]
                for multiplier in (2, 4, 8):
                    spacing = native * multiplier
                    self.profile_edit_spacing.addItem(
                        f"{multiplier}x sample spacing", spacing)
            self.profile_edit_spacing.setToolTip(
                "Choose editable anchors along the current X axis. Samples "
                "between anchors are linearly interpolated.")

        # Preserve an equivalent spacing when possible, otherwise default to
        # All samples. This keeps profile/curve switching predictable.
        best_index = 0
        if previous_spacing > 0.0:
            best_error = None
            for index in range(1, self.profile_edit_spacing.count()):
                spacing = float(self.profile_edit_spacing.itemData(index) or 0.0)
                error = abs(spacing - previous_spacing)
                if best_error is None or error < best_error:
                    best_error = error
                    best_index = index
        self.profile_edit_spacing.setCurrentIndex(best_index)
        self.profile_edit_spacing.setEnabled(True)
        self.profile_edit_spacing.blockSignals(False)
        self.profile_plot.set_edit_spacing_nm(
            float(self.profile_edit_spacing.currentData() or 0.0))

        self.profile_reset_curve_button.setEnabled(False)
        self.profile_reset_profile_button.setEnabled(
            self._runtime_resources is not None)


    @Slot(bool)
    def _realtime_metal_toggled(self, enabled: bool):
        self._metal_preview_generation += 1
        self._metal_preview_pending = False
        self._metal_preview_timer.stop()

        if not enabled:
            return

        try:
            if self._metal_preview is None:
                self._metal_preview = filmviz.MetalPreview()
        except Exception as error:
            self.realtime_metal.blockSignals(True)
            self.realtime_metal.setChecked(False)
            self.realtime_metal.setEnabled(False)
            self.realtime_metal.blockSignals(False)
            QMessageBox.warning(
                self,
                "Realtime Metal unavailable",
                str(error))
            return

        self._schedule_metal_preview()

    @Slot()
    def _schedule_metal_preview(self, *_):
        if not self.realtime_metal.isChecked():
            return

        # Every interaction invalidates the generation that is currently
        # rendering. If Metal is busy, keep exactly one pending request for the
        # newest state; the completion callback immediately renders that latest
        # state. This makes curve dragging interactive without queueing every
        # intermediate mouse-move.
        self._metal_preview_generation += 1
        self._metal_preview_pending = True
        if self._memory_log is not None:
            self._memory_log.record(
                "preview_scheduled", generation=self._metal_preview_generation,
                busy=self._metal_preview_busy)

        if self._metal_preview_busy:
            return

        self._metal_preview_timer.start()

    def _schedule_profile_metal_preview(self):
        self._metal_profiles_dirty = True
        self._schedule_metal_preview()

    def _metal_settings(self):
        settings = self._common()
        settings.update(
            negative_grain=self.negative_grain.value() if self.grain_enabled.isChecked() else 0.0,
            print_grain=self.print_grain.value() if self.grain_enabled.isChecked() else 0.0,
            grain_size=self.grain_size.value(),
            grain_chroma=self.grain_chroma.value(),
            grain_tonal_enabled=self.grain_tonal_enabled.isChecked(),
            grain_shadows=self.grain_shadows.value(),
            grain_midtones=self.grain_midtones.value(),
            grain_highlights=self.grain_highlights.value(),
            grain_seed=self.grain_seed.value(),
            film_format=self.film_format.currentData(),
            image_width_mm=self.image_width_mm.value(),
            negative_mtf=self.negative_mtf.value() * 0.01,
            print_mtf=self.print_mtf.value() * 0.01,
            halation_strength=self.halation_strength.value() if self.halation_enabled.isChecked() else 0.0,
            halation_radius=self.halation_radius.value(),
            halation_threshold=self.halation_threshold.value(),
        )
        settings.pop("resources", None)
        settings.pop("lut_size", None)
        settings.pop("use_lut_acceleration", None)
        settings.pop("threads", None)
        return settings

    @Slot()
    def _start_metal_preview(self):
        if not self.realtime_metal.isChecked():
            self._metal_preview_pending = False
            return

        if self._metal_preview_busy:
            # Do not lose an edit that arrives while Metal is rendering.
            self._metal_preview_pending = True
            return

        input_filename = str(
            Path(self.input_image.value()).expanduser().resolve())
        if not Path(input_filename).is_file():
            self.stage.setText("Realtime Metal input is unavailable")
            return

        if self._metal_preview is None:
            try:
                self._metal_preview = filmviz.MetalPreview()
            except Exception as error:
                self.stage.setText(f"Realtime Metal unavailable: {error}")
                return

        generation = self._metal_preview_generation
        resources = str(self._effective_resources_path())
        settings = self._metal_settings()
        invalidate_profiles = self._metal_profiles_dirty
        self._metal_profiles_dirty = False
        self._metal_preview_busy = True
        self._metal_preview_pending = False
        self.stage.setText("Rendering Metal preview…")
        if self._memory_log is not None:
            self._memory_log.record("render_started", generation=generation)

        def render_preview():
            try:
                if invalidate_profiles:
                    self._metal_preview.invalidate_profiles()
                result = self._metal_preview.render(
                    input_filename=input_filename,
                    resources=resources,
                    settings=settings,
                    max_dimension=1280,
                    time=0.0)
            except Exception:
                if self._memory_log is not None:
                    self._memory_log.record("render_failed", generation=generation)
                self._metal_preview_bridge.failed.emit(
                    generation,
                    traceback.format_exc())
            else:
                if self._memory_log is not None:
                    self._memory_log.record(
                        "render_returned", generation=generation,
                        display_bytes=len(result["rgb"]),
                        scope_bytes=len(result["scope_rgb"]),
                        metal_bytes_before=result["metal_bytes_before"],
                        metal_bytes_configured=result["metal_bytes_configured"],
                        metal_bytes_submitted=result["metal_bytes_submitted"],
                        metal_bytes_fenced=result["metal_bytes_fenced"],
                        metal_bytes_after_pool=result["metal_bytes_after_pool"])
                self._metal_preview_bridge.finished.emit(
                    generation,
                    result)

        threading.Thread(
            target=render_preview,
            name="FilmVizMetalPreview",
            daemon=True).start()

    @Slot(int, object)
    def _metal_preview_finished(self, generation: int, result):
        self._metal_preview_busy = False
        if self._memory_log is not None:
            self._memory_log.record(
                "result_received", generation=generation,
                accepted=(self.realtime_metal.isChecked()
                    and generation == self._metal_preview_generation))

        if (
            self.realtime_metal.isChecked()
            and generation == self._metal_preview_generation
        ):
            width = int(result["width"])
            height = int(result["height"])
            rgb = bytes(result["rgb"])
            scope_rgb = array.array("f")
            scope_rgb.frombytes(bytes(result["scope_rgb"]))
            identity = (
                f"metal:{Path(self.input_image.value()).expanduser().resolve()}")

            self.image_preview.set_rgb(
                width,
                height,
                rgb,
                image_identity=identity)
            self._scope_width = width
            self._scope_height = height
            self._scope_rgb = scope_rgb
            self.left_scope.set_rgb(width, height, scope_rgb)
            self.right_scope.set_rgb(width, height, scope_rgb)
            self._resample_probes()
            self.stage.setText("Realtime Metal preview")
            if self._memory_log is not None:
                self._memory_log.record("preview_applied", generation=generation)

        if self._metal_preview_pending and self.realtime_metal.isChecked():
            # Render the newest UI/profile state immediately. Intermediate
            # edits are deliberately collapsed into this single latest-state
            # render so dragging remains responsive.
            self._metal_preview_timer.start(0)

    @Slot()
    def _sample_preview_memory(self):
        image = self.image_preview._image
        scope = self._scope_rgb
        self._memory_log.record(
            "periodic", generation=self._metal_preview_generation,
            busy=self._metal_preview_busy, pending=self._metal_preview_pending,
            image_bytes=(image.sizeInBytes() if not image.isNull() else 0),
            scope_bytes=(len(scope) * scope.itemsize if scope is not None else 0),
            metal_device_bytes=(self._metal_preview.metal_allocated_bytes()
                if self._metal_preview is not None else 0))

    @Slot(int, str)
    def _metal_preview_failed(self, generation: int, message: str):
        self._metal_preview_busy = False
        if self._memory_log is not None:
            self._memory_log.record("error_received", generation=generation)
        if generation == self._metal_preview_generation:
            detail = message.strip().splitlines()
            self.stage.setText(
                detail[-1] if detail else "Realtime Metal preview failed")
        if self._metal_preview_pending and self.realtime_metal.isChecked():
            # Render the newest UI/profile state immediately. Intermediate
            # edits are deliberately collapsed into this single latest-state
            # render so dragging remains responsive.
            self._metal_preview_timer.start(0)

    # Explicit allowlists keep paths, transient UI state and profile-curve
    # edits out of look presets. Store actual strengths even when bypassed.
    _preset_numbers = (
        "exposure", "negative_flash", "print_flash", "push_pull",
        "color_density", "color_depth", "negative_bleach_bypass", "print_bleach_bypass",
        "printer_light_red", "printer_light_green", "printer_light_blue", "printer_light_master",
        "middle_gray", "printer_temperature", "negative_grain", "print_grain",
        "grain_size", "grain_chroma", "grain_seed", "grain_shadows", "grain_midtones",
        "grain_highlights", "image_width_mm", "negative_mtf", "print_mtf",
        "halation_strength", "halation_radius", "halation_threshold")
    _preset_switches = ("grain_enabled", "halation_enabled", "grain_tonal_enabled", "response_bypass")
    _preset_combos = ("negative_profile", "print_profile", "film_format")

    def _read_presets(self):
        raw = self._preset_settings.value("look_presets_v1", "{}")
        presets = json.loads(raw)
        if not isinstance(presets, dict):
            raise ValueError("Invalid preset storage")
        return presets

    def _refresh_presets(self, selected=None):
        try:
            presets = self._read_presets()
        except (ValueError, TypeError) as error:
            QMessageBox.warning(self, "Presets", str(error))
            return
        self.preset_selector.clear()
        for name in sorted(presets, key=str.casefold):
            self.preset_selector.addItem(name)
        if selected is not None:
            self.preset_selector.setCurrentText(selected)

    def _write_presets(self, presets, selected=None):
        self._preset_settings.setValue("look_presets_v1", json.dumps(presets, allow_nan=False))
        self._preset_settings.sync()
        if self._preset_settings.status() != QSettings.Status.NoError:
            raise OSError("Could not save application presets")
        self._refresh_presets(selected)

    @Slot()
    def _save_preset(self):
        name, accepted = QInputDialog.getText(
            self, "Save preset", "Preset name:", text=self.preset_selector.currentText())
        name = name.strip()
        if not accepted or not name:
            return
        try:
            presets = self._read_presets()
            if name in presets and QMessageBox.question(
                    self, "Replace preset", f'Replace "{name}"?',
                    QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                    QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes:
                return
            values = {key: getattr(self, key).value() for key in self._preset_numbers}
            values.update({key: getattr(self, key).isChecked() for key in self._preset_switches})
            values.update({key: getattr(self, key).currentData() for key in self._preset_combos})
            values.update(input_profile=self.input_profile.currentText(),
                          output_profile=self.output_profile.currentText(),
                          color_response={key: control.value() for key, control in self.response_controls.items()})
            presets[name] = {"version": 1, "values": values}
            self._write_presets(presets, name)
        except (ValueError, TypeError, OSError) as error:
            QMessageBox.warning(self, "Save preset", str(error))

    @Slot()
    def _load_preset(self):
        name = self.preset_selector.currentText()
        if not name:
            return
        try:
            preset = self._read_presets()[name]
            if preset.get("version") != 1:
                raise ValueError("Unsupported preset version")
            values = preset["values"]
            # Validate everything before changing controls.
            controls = [(getattr(self, key), values[key]) for key in self._preset_numbers]
            controls += [(control, values["color_response"][key])
                         for key, control in self.response_controls.items()]
            for control, value in controls:
                if (type(value) not in (int, float) or not math.isfinite(value)
                        or not control.minimum() <= value <= control.maximum()
                        or (isinstance(control, QSpinBox) and int(value) != value)):
                    raise ValueError("Preset has an invalid numeric setting")
            for key in self._preset_switches:
                if type(values[key]) is not bool:
                    raise ValueError("Preset has an invalid switch setting")
            for key in self._preset_combos:
                if getattr(self, key).findData(values[key]) < 0:
                    raise ValueError(f"Unavailable preset option: {key}")
            for key in ("input_profile", "output_profile"):
                if not any(e["profile"] == values[key] for e in getattr(self, key)._entries):
                    raise ValueError(f"Unavailable color profile: {values[key]}")
        except (KeyError, AttributeError, ValueError, TypeError) as error:
            QMessageBox.warning(self, "Load preset", str(error))
            return
        realtime = self.realtime_metal.isChecked()
        self.realtime_metal.setChecked(False)
        try:
            # Profile/format callbacks set defaults; restore numeric values last.
            for key in self._preset_combos:
                control = getattr(self, key)
                control.setCurrentIndex(control.findData(values[key]))
            for key in ("input_profile", "output_profile"):
                getattr(self, key).setCurrentText(values[key])
            for control, value in controls:
                control.setValue(int(value) if isinstance(control, QSpinBox) else value)
            for key in self._preset_switches:
                getattr(self, key).setChecked(values[key])
        finally:
            self.realtime_metal.setChecked(realtime)
        self._schedule_metal_preview()

    @Slot()
    def _delete_preset(self):
        name = self.preset_selector.currentText()
        if not name:
            return
        if QMessageBox.question(self, "Delete preset", f'Delete "{name}"?',
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes:
            return
        try:
            presets = self._read_presets()
            presets.pop(name, None)
            self._write_presets(presets)
        except (ValueError, TypeError, OSError) as error:
            QMessageBox.warning(self, "Delete preset", str(error))

    def _common(self):
        return dict(
            resources=str(self._effective_resources_path()),
            input=self.input_profile.currentText(),
            negative=(
                self.negative_profile.currentData()
                or self.negative_profile.currentText()),
            print=self.print_profile.currentData() or self.print_profile.currentText(),
            output=self.output_profile.currentText(),
            lut_size=self.lut_size.value(),
            use_lut_acceleration=self.use_lut_acceleration.isChecked(),
            exposure=self.exposure.value(),
            negative_flash=self.negative_flash.value(),
            print_flash=self.print_flash.value(),
            push_pull=self.push_pull.value(),
            color_density=self.color_density.value(),
            color_depth=self.color_depth.value(),
            color_response=self._response_settings(),
            negative_bleach_bypass=self.negative_bleach_bypass.value(),
            print_bleach_bypass=self.print_bleach_bypass.value(),
            printer_light_red=self.printer_light_red.value(),
            printer_light_green=self.printer_light_green.value(),
            printer_light_blue=self.printer_light_blue.value(),
            printer_light_master=self.printer_light_master.value(),
            middle_gray=self.middle_gray.value(),
            printer_temperature=self.printer_temperature.value(),
            threads=self.threads.value(),
        )

    @Slot()
    def _convert_from_shortcut(self):
        focus = QApplication.focusWidget()

        # Keep Enter usable for committing text/numeric edits. Everywhere else
        # Return/Enter is the global "Convert image" shortcut.
        if isinstance(focus, (QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit)):
            return

        if self.convert_button.isEnabled():
            self.convert_image()

    @Slot()
    def convert_image(self):
        arguments = self._common()
        arguments.update(
            input_filename=self.input_image.value(),
            output_filename=self.output_image.value(),
            negative_grain=self.negative_grain.value() if self.grain_enabled.isChecked() else 0.0,
            print_grain=self.print_grain.value() if self.grain_enabled.isChecked() else 0.0,
            grain_size=self.grain_size.value(),
            grain_chroma=self.grain_chroma.value(),
            grain_tonal_enabled=self.grain_tonal_enabled.isChecked(),
            grain_shadows=self.grain_shadows.value(),
            grain_midtones=self.grain_midtones.value(),
            grain_highlights=self.grain_highlights.value(),
            grain_seed=self.grain_seed.value(),
            film_format=self.film_format.currentData(),
            image_width_mm=self.image_width_mm.value(),
            negative_mtf=self.negative_mtf.value() * 0.01,
            print_mtf=self.print_mtf.value() * 0.01,
            halation_strength=self.halation_strength.value() if self.halation_enabled.isChecked() else 0.0,
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
        arguments.pop("use_lut_acceleration", None)
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
            if path.is_file():
                self._load_diagnostics(str(path))

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
    surface_format = QSurfaceFormat.defaultFormat()
    surface_format.setColorSpace(APP_COLOR_SPACE)
    QSurfaceFormat.setDefaultFormat(surface_format)

    application = QApplication(sys.argv)
    application.setApplicationName("FilmViz")
    application.setApplicationDisplayName("FilmViz")

    # Keep the entire application visually compact. Apply the reduction once
    # at QApplication level so diagnostics, toolbars, tabs, controls, status
    # text, and the settings panel all use the same font size.
    application_font = application.font()
    application_font.setPointSize(
        max(7, application_font.pointSize() - 2))
    application.setFont(application_font)

    window = FilmVizWindow()

    if os.environ.get("FILMVIZ_APP_SMOKE_TEST") == "1":
        print("FilmViz Python application smoke test passed")
        return 0

    window.showMaximized()
    return application.exec()


if __name__ == "__main__":
    raise SystemExit(main())
