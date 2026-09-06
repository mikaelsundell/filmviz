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

    from PySide6.QtCore import QEvent, QObject, QPointF, QRectF, QThread, QTimer, QUrl, Qt, Signal, Slot
    from PySide6.QtGui import (
        QColor,
        QColorSpace,
        QDesktopServices,
        QImage,
        QPainter,
        QPainterPath,
        QPen,
        QPixmap,
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
        QLineEdit,
        QMainWindow,
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

    def value(self) -> str:
        return self.edit.text().strip()


def _double(value, minimum, maximum, step=0.1, decimals=3):
    widget = QDoubleSpinBox()
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



class ImagePreviewWidget(QWidget):
    probeRequested = Signal(float, float)

    def __init__(self):
        super().__init__()
        self._image = QImage()
        self._message = "Convert an image to preview the result"
        self._probe = None
        self._color_space = APP_COLOR_SPACE

        self._fit_to_view = True
        self._zoom = 1.0
        self._pan = QPointF(0.0, 0.0)

        self.setMinimumSize(520, 320)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_AcceptTouchEvents, True)

    def set_rgb(self, width: int, height: int, rgb: bytes):
        image = QImage(
            rgb,
            width,
            height,
            width * 3,
            QImage.Format.Format_RGB888)
        image.setColorSpace(self._color_space)
        self._image = image.copy()
        self._message = ""
        self._probe = None
        self.fit_to_view()
        self.update()

    def set_color_space(self, color_space):
        self._color_space = color_space
        if not self._image.isNull():
            self._image.setColorSpace(color_space)
        self.update()

    def set_error(self, message: str):
        self._image = QImage()
        self._message = message
        self._probe = None
        self._fit_to_view = True
        self._zoom = 1.0
        self._pan = QPointF(0.0, 0.0)
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

    def mousePressEvent(self, event):
        if self._image.isNull():
            return

        # Pixel probing is intentionally restricted to a real left mouse
        # button click. Ignore mouse events synthesized by macOS from the
        # trackpad so taps and navigation gestures do not create probe data.
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
        self._probe = (u, v)
        self.update()
        self.probeRequested.emit(float(u), float(v))
        event.accept()

    def wheelEvent(self, event):
        if self._image.isNull():
            event.ignore()
            return

        pixel_delta = event.pixelDelta()
        angle_delta = event.angleDelta()
        modifiers = event.modifiers()

        # On macOS, two-finger trackpad scrolling normally arrives here.
        # Prefer pixelDelta when available because it gives smooth panning.
        if not pixel_delta.isNull():
            self._fit_to_view = False
            self._pan += QPointF(
                pixel_delta.x(),
                pixel_delta.y())
            self.update()
            event.accept()
            return

        # Some Qt/macOS combinations only expose angleDelta for trackpad
        # scrolling. Treat it as pan unless the user explicitly holds
        # Ctrl/Command, in which case it becomes wheel zoom.
        zoom_modifier = (
            modifiers & Qt.KeyboardModifier.ControlModifier
            or modifiers & Qt.KeyboardModifier.MetaModifier
        )

        if not angle_delta.isNull():
            if zoom_modifier:
                factor = 1.15 ** (angle_delta.y() / 120.0)
                self._zoom_at(event.position(), factor)
            else:
                self._fit_to_view = False
                self._pan += QPointF(
                    angle_delta.x() / 2.0,
                    angle_delta.y() / 2.0)
                self.update()

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

        if self._probe is not None:
            u, v = self._probe
            x = rect.left() + u * rect.width()
            y = rect.top() + v * rect.height()

            painter.setPen(QPen(QColor(245, 210, 70), 1.0))
            painter.drawEllipse(QPointF(x, y), 6.0, 6.0)
            painter.drawLine(QPointF(x - 10, y), QPointF(x + 10, y))
            painter.drawLine(QPointF(x, y - 10), QPointF(x, y + 10))


class VectorScopeWidget(QWidget):
    def __init__(self):
        super().__init__()
        self._samples = []
        self._probe_rgb = None
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
        self._probe_rgb = rgb
        self.update()

    def clear_probe(self):
        self._probe_rgb = None
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

        if self._probe_rgb is not None:
            _, cb, cr = self._ycbcr(*self._probe_rgb)
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


class HistogramWidget(QWidget):
    def __init__(self):
        super().__init__()
        self._histograms = None
        self._probe_rgb = None
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
        self._probe_rgb = rgb
        self.update()

    def clear_probe(self):
        self._probe_rgb = None
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

        if self._probe_rgb is not None:
            painter.setPen(QPen(QColor(245, 210, 70), 1.3))
            for value in self._probe_rgb:
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
        self._probe = None
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
        self._probe = (u, rgb)
        self.update()

    def clear_probe(self):
        self._probe = None
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

        if self._probe is not None:
            u, rgb = self._probe
            probe_pen = QPen(QColor(245, 210, 70), 1.8)
            painter.setBrush(QColor(245, 210, 70))
            painter.setPen(probe_pen)
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
        self._probe = None
        self._mode = "rgb"
        self.setMinimumSize(240, 200)
        self.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)

    def set_rgb(self, width: int, height: int, rgb):
        columns = 384
        bins = 512
        pixel_count = width * height
        stride = max(1, pixel_count // 220000)

        if np is not None:
            values = np.asarray(rgb, dtype=np.float32).reshape(-1, 3)
            indices = np.arange(0, pixel_count, stride, dtype=np.int64)
            sampled = values[indices]

            x = indices % width
            column_indices = np.minimum(
                columns - 1,
                (x * columns // max(1, width)).astype(np.int64))

            rgb_bins = np.clip(
                (sampled * (bins - 1)).astype(np.int64),
                0,
                bins - 1)

            waveform = np.zeros(
                (3, columns, bins),
                dtype=np.int32)

            for channel in range(3):
                np.add.at(
                    waveform[channel],
                    (column_indices, rgb_bins[:, channel]),
                    1)

            luma = (
                0.2126 * sampled[:, 0]
                + 0.7152 * sampled[:, 1]
                + 0.0722 * sampled[:, 2])
            luma_bins = np.clip(
                (luma * (bins - 1)).astype(np.int64),
                0,
                bins - 1)

            luma_waveform = np.zeros(
                (columns, bins),
                dtype=np.int32)
            np.add.at(
                luma_waveform,
                (column_indices, luma_bins),
                1)

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

        for pixel in range(0, pixel_count, stride):
            x = pixel % width
            column = min(
                columns - 1,
                int(x * columns / max(1, width)))
            offset = pixel * 3

            r = float(rgb[offset])
            g = float(rgb[offset + 1])
            b = float(rgb[offset + 2])

            for channel, value in enumerate((r, g, b)):
                bin_index = min(
                    bins - 1,
                    max(0, int(value * (bins - 1))))
                waveform[channel][column][bin_index] += 1

            y = 0.2126 * r + 0.7152 * g + 0.0722 * b
            y_bin = min(
                bins - 1,
                max(0, int(y * (bins - 1))))
            luma_waveform[column][y_bin] += 1

        self._waveform = waveform
        self._luma_waveform = luma_waveform
        self.update()

    def set_mode(self, mode: str):
        self._mode = mode if mode in ("rgb", "y") else "rgb"
        self.update()

    def set_probe(self, u: float, v: float, rgb):
        self._probe = (u, rgb)
        self.update()

    def clear_probe(self):
        self._probe = None
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

        painter.setPen(Qt.PenStyle.NoPen)

        if self._mode == "y":
            if self._luma_waveform is None:
                return

            maximum = max(
                int(max(column))
                for column in self._luma_waveform)

            if maximum <= 0:
                return

            for x_index, column in enumerate(self._luma_waveform):
                px = (
                    plot.left()
                    + x_index / max(1, len(self._luma_waveform) - 1)
                    * plot.width())

                for value, count in enumerate(column):
                    if count <= 0:
                        continue

                    alpha = min(
                        0.78,
                        0.08
                        + 0.70
                        * ((count / maximum) ** 0.32))

                    color = QColor(220, 220, 220)
                    color.setAlphaF(alpha)
                    painter.setBrush(color)

                    py = (
                        plot.bottom()
                        - value / max(1, len(column) - 1)
                        * plot.height())

                    painter.drawRect(
                        QRectF(px, py, 1.2, 1.2))

            if self._probe is not None:
                u, rgb = self._probe
                y = (
                    0.2126 * rgb[0]
                    + 0.7152 * rgb[1]
                    + 0.0722 * rgb[2])
                px = plot.left() + u * plot.width()
                py = plot.bottom() - y * plot.height()
                painter.setBrush(QColor(245, 210, 70))
                painter.setPen(QPen(QColor(245, 210, 70), 1.8))
                painter.drawEllipse(QPointF(px, py), 4.5, 4.5)

            title = "Y waveform"

        else:
            colors = (
                QColor("#ef5555"),
                QColor("#55c477"),
                QColor("#5b8def"),
            )

            maxima = [
                max(int(max(column)) for column in channel)
                for channel in self._waveform
            ]
            maximum = max(maxima)

            if maximum <= 0:
                return

            for channel, data in enumerate(self._waveform):
                for x_index, column in enumerate(data):
                    px = (
                        plot.left()
                        + x_index / max(1, len(data) - 1)
                        * plot.width())

                    for value, count in enumerate(column):
                        if count <= 0:
                            continue

                        alpha = min(
                            0.72,
                            0.06
                            + 0.66
                            * ((count / maximum) ** 0.32))

                        color = QColor(colors[channel])
                        color.setAlphaF(alpha)
                        painter.setBrush(color)

                        py = (
                            plot.bottom()
                            - value / max(1, len(column) - 1)
                            * plot.height())

                        painter.drawRect(
                            QRectF(px, py, 1.2, 1.2))

            if self._probe is not None:
                u, rgb = self._probe
                px = plot.left() + u * plot.width()
                painter.setBrush(QColor(245, 210, 70))
                painter.setPen(QPen(QColor(245, 210, 70), 1.8))
                for value in rgb:
                    py = plot.bottom() - value * plot.height()
                    painter.drawEllipse(QPointF(px, py), 4.5, 4.5)

            title = "RGB waveform"

        painter.setPen(self.palette().color(self.foregroundRole()))
        painter.drawText(
            QRectF(
                plot.left(),
                plot.bottom() + 2,
                plot.width(),
                label_height),
            Qt.AlignmentFlag.AlignCenter,
            title)


class ScopePane(QWidget):
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

        toolbar_layout.addWidget(self.selector, 1)
        toolbar_layout.addWidget(self.zoom2, 0)

        self.stack = QStackedWidget()
        self.stack.setSizePolicy(
            QSizePolicy.Policy.Expanding,
            QSizePolicy.Policy.Expanding)

        self.vector_scope = VectorScopeWidget()
        self.histogram = HistogramWidget()
        self.parade = ParadeWidget()
        self.waveform = WaveformWidget()

        self.stack.addWidget(self.vector_scope)
        self.stack.addWidget(self.histogram)
        self.stack.addWidget(self.parade)
        self.stack.addWidget(self.waveform)

        layout.addWidget(toolbar)
        layout.addWidget(self.stack, 1)

        index = self.selector.findText(initial)
        if index >= 0:
            self.selector.setCurrentIndex(index)

        self.selector.currentIndexChanged.connect(
            self._mode_changed)
        self.zoom2.toggled.connect(
            self.vector_scope.set_zoom2)

        self._mode_changed(
            self.selector.currentIndex())

    def _mode_changed(self, index: int):
        if index == 4:
            self.stack.setCurrentIndex(3)
            self.waveform.set_mode("y")
        else:
            self.stack.setCurrentIndex(index)
            if index == 3:
                self.waveform.set_mode("rgb")

        self.zoom2.setVisible(index == 0)

    def set_rgb(self, width: int, height: int, rgb):
        # Populate every scope once so changing the dropdown is instantaneous.
        self.vector_scope.set_rgb(width, height, rgb)
        self.histogram.set_rgb(width, height, rgb)
        self.parade.set_rgb(width, height, rgb)
        self.waveform.set_rgb(width, height, rgb)

    def set_probe(self, u: float, v: float, rgb):
        self.vector_scope.set_probe(u, v, rgb)
        self.histogram.set_probe(u, v, rgb)
        self.parade.set_probe(u, v, rgb)
        self.waveform.set_probe(u, v, rgb)

    def clear_probe(self):
        self.vector_scope.clear_probe()
        self.histogram.clear_probe()
        self.parade.clear_probe()
        self.waveform.clear_probe()


class FilmVizWindow(QMainWindow):
    def __init__(self):
        super().__init__()
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
        display_toolbar_layout = QHBoxLayout(display_toolbar)
        display_toolbar_layout.setContentsMargins(0, 0, 0, 0)
        display_toolbar_layout.setSpacing(6)
        display_toolbar_layout.addStretch(1)
        display_label = QLabel("Display: Rec.709 Gamma 2.4")
        display_label.setToolTip(
            "FilmViz preview and application surface are configured for "
            "Rec.709 Gamma 2.4.")
        display_toolbar_layout.addWidget(display_label)
        diagnostics_layout.addWidget(display_toolbar, 0)

        diagnostics_splitter = QSplitter(Qt.Orientation.Vertical)
        diagnostics_splitter.setChildrenCollapsible(False)

        self.image_preview = ImagePreviewWidget()
        self.image_preview.probeRequested.connect(
            self._probe_image_pixel)
        diagnostics_splitter.addWidget(self.image_preview)

        self.probe_output = QPlainTextEdit()
        self.probe_output.setReadOnly(True)
        self.probe_output.setMinimumHeight(70)
        self.probe_output.setPlaceholderText(
            "Click the converted image to inspect the matching source pixel "
            "through AP0 → spectrum → negative → print → output.")
        diagnostics_splitter.addWidget(self.probe_output)

        scopes = QSplitter(Qt.Orientation.Horizontal)
        self.left_scope = ScopePane("Vectorscope")
        self.right_scope = ScopePane("RGB Histogram")
        scopes.addWidget(self.left_scope)
        scopes.addWidget(self.right_scope)
        scopes.setStretchFactor(0, 1)
        scopes.setStretchFactor(1, 1)
        scopes.setChildrenCollapsible(False)
        diagnostics_splitter.addWidget(scopes)

        diagnostics_splitter.setStretchFactor(0, 4)
        diagnostics_splitter.setStretchFactor(1, 1)
        diagnostics_splitter.setStretchFactor(2, 3)
        diagnostics_splitter.setSizes([470, 110, 290])
        diagnostics_layout.addWidget(diagnostics_splitter, 1)

        panel = QWidget()
        panel.setMinimumWidth(390)
        panel.setSizePolicy(
            QSizePolicy.Policy.Preferred,
            QSizePolicy.Policy.Expanding)
        root_layout = QVBoxLayout(panel)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(6)

        compact_font = panel.font()
        compact_font.setPointSize(max(9, compact_font.pointSize() - 2))
        panel.setFont(compact_font)
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

        profiles = filmviz.profiles()
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
        common.setMaximumWidth(430)
        common.setSizePolicy(
            QSizePolicy.Policy.Preferred,
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

        self.input_profile = QComboBox()
        self.input_profile.addItems(profiles["input"])
        common_form.addRow("Input profile", self.input_profile)

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

        self.output_profile = QComboBox()
        self.output_profile.addItems(profiles["output"])
        self.output_profile.setCurrentText("rec709-gamma24")
        common_form.addRow("Output profile", self.output_profile)

        self.exposure = _double(0.0, -10.0, 10.0, 0.25)
        self.negative_flash = _double(0.0, 0.0, 25.0, 0.1, 2)
        self.print_flash = _double(0.0, 0.0, 25.0, 0.1, 2)
        self.push_pull = _double(0.0, -5.0, 5.0, 0.25)
        self.negative_bleach_bypass = _double(0.0, 0.0, 1.0, 0.05)
        self.print_bleach_bypass = _double(0.0, 0.0, 1.0, 0.05)
        self.printer_light_red = _double(25.0, 0.0, 50.0, 0.1, 1)
        self.printer_light_green = _double(25.0, 0.0, 50.0, 0.1, 1)
        self.printer_light_blue = _double(25.0, 0.0, 50.0, 0.1, 1)
        self.printer_light_master = _double(0.0, -10.0, 10.0, 0.05, 2)
        self.middle_gray = _double(0.18, 0.001, 2.0, 0.01, 4)
        self.printer_temperature = _double(3200.0, 1000.0, 10000.0, 50.0, 0)
        self.lut_size = QSpinBox()
        self.lut_size.setRange(2, 129)
        self.lut_size.setValue(65)
        self.use_lut_acceleration = QCheckBox()
        self.use_lut_acceleration.setChecked(True)
        self.use_lut_acceleration.setToolTip(
            "Disable to evaluate the spectral FilmPipeline directly per pixel. "
            "Direct mode is intended for validation and requires grain and "
            "halation to be disabled.")
        self.threads = QSpinBox()
        self.threads.setRange(0, 256)
        self.threads.setSpecialValueText("Auto")
        self.threads.setValue(0)

        self.exposure_control = SliderSpinRow(self.exposure)
        self.negative_flash_control = SliderSpinRow(self.negative_flash)
        self.print_flash_control = SliderSpinRow(self.print_flash)
        self.push_pull_control = SliderSpinRow(self.push_pull)
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
        controls.setMaximumWidth(400)
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
        pipeline_tabs.setMaximumWidth(430)
        pipeline_tabs.setSizePolicy(
            QSizePolicy.Policy.Preferred,
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
            0,
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop)
        pipeline_scroll_host_layout.addStretch(1)

        pipeline_scroll.setWidget(pipeline_scroll_host)
        pipeline_page_layout.addWidget(pipeline_scroll)
        pipeline_tabs.addTab(pipeline_page, "Pipeline")
        controls_splitter.addWidget(pipeline_tabs)

        self.tabs = QTabWidget()
        self.tabs.setMinimumWidth(360)
        self.tabs.setMaximumWidth(430)
        self.tabs.setSizePolicy(
            QSizePolicy.Policy.Preferred,
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
        self.negative_grain = _double(0.0, 0.0, 10.0)
        self.print_grain = _double(0.0, 0.0, 10.0)
        self.grain_size = _double(1.0, 1.0, 32.0, 0.25)
        self.grain_chroma = _double(1.0, 0.0, 4.0, 0.1)
        self.grain_seed = QSpinBox()
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
        self.negative_mtf = _double(0.0, 0.0, 200.0, 5.0, 1)
        self.print_mtf = _double(0.0, 0.0, 200.0, 5.0, 1)
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
        image_form.addRow("Negative grain", self.negative_grain_control)
        image_form.addRow("Print grain", self.print_grain_control)
        image_form.addRow("Grain scale (px)", self.grain_size_control)
        image_form.addRow("Grain chroma", self.grain_chroma_control)
        image_form.addRow("Grain seed", self.grain_seed)
        image_form.addRow("Film format", self.film_format)
        image_form.addRow("Active image width (mm)", self.image_width_mm)
        image_form.addRow("Negative MTF", self.negative_mtf_control)
        image_form.addRow("Print MTF", self.print_mtf_control)
        image_form.addRow("Halation", self.halation_strength_control)
        image_form.addRow("Halation radius (px)", self.halation_radius_control)
        image_form.addRow("Halation threshold", self.halation_threshold_control)
        self.convert_button = QPushButton("Convert image")
        self.convert_button.clicked.connect(self.convert_image)
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
        self.profiles_reset_button = _reset_button()
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



    @Slot()
    def _reset_pipeline(self):
        self.input_profile.setCurrentIndex(0)
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
        self.negative_grain.setValue(0.0)
        self.print_grain.setValue(0.0)
        self.grain_size.setValue(1.0)
        self.grain_chroma.setValue(1.0)
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
        if self.profile_family.count() > 0:
            self.profile_family.setCurrentIndex(0)
        if self.profile_curve_type.count() > 0:
            self.profile_curve_type.setCurrentIndex(0)
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
                preview_rgb)
            self._scope_width = 0
            self._scope_height = 0
            self._scope_rgb = None
            self.left_scope.clear_probe()
            self.right_scope.clear_probe()
            QMessageBox.warning(
                self,
                "High-precision scopes unavailable",
                str(error))
            return

        self.image_preview.set_rgb(
            preview_width,
            preview_height,
            preview_rgb)

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
        self.left_scope.clear_probe()
        self.right_scope.clear_probe()


    @Slot(float, float)
    def _probe_image_pixel(self, u: float, v: float):
        scope_rgb = _scope_rgb_at(
            self._scope_width,
            self._scope_height,
            self._scope_rgb,
            u,
            v)
        if scope_rgb is not None:
            self.left_scope.set_probe(u, v, scope_rgb)
            self.right_scope.set_probe(u, v, scope_rgb)

        try:
            arguments = self._common()
            probe = filmviz.probe_image_pixel(
                resources=arguments["resources"],
                input_filename=self.input_image.value(),
                input=arguments["input"],
                negative=arguments["negative"],
                print=arguments["print"],
                exposure=arguments["exposure"],
                negative_flash=arguments["negative_flash"],
                print_flash=arguments["print_flash"],
                push_pull=arguments["push_pull"],
                negative_bleach_bypass=
                    arguments["negative_bleach_bypass"],
                print_bleach_bypass=
                    arguments["print_bleach_bypass"],
                printer_light_red=
                    arguments["printer_light_red"],
                printer_light_green=
                    arguments["printer_light_green"],
                printer_light_blue=
                    arguments["printer_light_blue"],
                printer_light_master=
                    arguments["printer_light_master"],
                middle_gray=arguments["middle_gray"],
                printer_temperature=
                    arguments["printer_temperature"],
                u=u,
                v=v,
            )
        except Exception as error:
            self.probe_output.setPlainText(
                f"Probe failed:\n{error}")
            return

        def triplet(name):
            values = probe[name]
            return (
                f"({float(values[0]):.6g}, "
                f"{float(values[1]):.6g}, "
                f"{float(values[2]):.6g})"
            )

        spectrum = probe["spectrum"]
        spectral_text = " ".join(
            f"{w}:{float(spectrum[w]):.4g}"
            for w in (
                "400", "420", "440", "460", "500", "550",
                "600", "620", "640", "660", "680"
            )
        )

        self.probe_output.setPlainText(
            f"pixel ({probe['x']}, {probe['y']}) / "
            f"{probe['width']}×{probe['height']}\n"
            f"encoded RGB       {triplet('encoded_rgb')}\n"
            f"input AP0         {triplet('ap0_input')}\n"
            f"spectrum          {spectral_text}\n"
            f"negative H R/G/B  {triplet('negative_exposure')}\n"
            f"Status-M D R/G/B  {triplet('negative_status_m')}\n"
            f"calibrated D      {triplet('negative_calibrated')}\n"
            f"print H R/G/B     {triplet('print_exposure')}\n"
            f"print D R/G/B     {triplet('print_density')}\n"
            f"output AP0        {triplet('output_ap0')}\n"
            f"output Rec709     {triplet('output_rec709_gamma24')}"
        )

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

        path = (
            Path(self.resources.value())
            / profile_directory
            / filename
        )

        x_column = None
        y_columns = None
        stop_axis = None

        if filename.endswith("_spectral_dye_density_curves.csv"):
            x_column = "wavelength_nm"

            if family == "negative":
                # Camera-negative dye CSVs also contain scalar metadata such
                # as source_spacing_nm and working_spacing_nm. Those are file
                # metadata, not wavelength-varying curves; plotting them
                # produces the bogus horizontal lines at 10 and 5.
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
                # Both active camera-negative source tables carry camera stops
                # and LogE together with stop 0 anchored at -0.515 LogE.
                stop_axis = (-8.0, 8.0, -0.515)
            else:
                # Print sensitometry already uses log exposure as its
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
            film_format=self.film_format.currentData(),
            image_width_mm=self.image_width_mm.value(),
            negative_mtf=self.negative_mtf.value() * 0.01,
            print_mtf=self.print_mtf.value() * 0.01,
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
    window = FilmVizWindow()

    if os.environ.get("FILMVIZ_APP_SMOKE_TEST") == "1":
        print("FilmViz Python application smoke test passed")
        return 0

    window.showMaximized()
    return application.exec()


if __name__ == "__main__":
    raise SystemExit(main())
