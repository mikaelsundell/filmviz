#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""PySide6 desktop front end for the FilmViz Python bindings."""

from __future__ import annotations

import os
from pathlib import Path
import sys
import traceback


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
    from PySide6.QtCore import QObject, QThread, QUrl, Signal, Slot
    from PySide6.QtGui import QDesktopServices
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

    def __init__(self, operation, success_message: str):
        super().__init__()
        self.operation = operation
        self.success_message = success_message

    @Slot()
    def run(self):
        try:
            self.operation(self.progress.emit)
        except Exception:
            self.failed.emit(traceback.format_exc())
        else:
            self.finished.emit(self.success_message)


class PathRow(QWidget):
    def __init__(self, mode: str, initial: str = ""):
        super().__init__()
        self.mode = mode
        self.edit = QLineEdit(initial)
        self.edit.setMinimumWidth(520)
        self.edit.setSizePolicy(
            QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        button = QPushButton("Browse…")
        button.clicked.connect(self._browse)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.edit)
        layout.addWidget(button)
        layout.setStretch(0, 1)

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


class FilmVizWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("FilmViz — Experimental Spectral Film Processor")
        self.resize(980, 700)
        self._thread = None
        self._worker = None
        self._pending_output_image = None
        self._last_output_image = None

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
            ("Middle gray", self.middle_gray),
            ("Printer K", self.printer_temperature),
            ("LUT size", self.lut_size),
            ("Worker threads", self.threads),
        )):
            column = row % 2
            grid_row = row // 2
            controls_layout.addWidget(QLabel(label), grid_row, column * 2)
            controls_layout.addWidget(widget, grid_row, column * 2 + 1)
        common_form.addRow(controls)
        root_layout.addWidget(common)

        tabs = QTabWidget()
        root_layout.addWidget(tabs)

        image_tab = QWidget()
        image_form = QFormLayout(image_tab)
        image_form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        default_image = PROJECT_ROOT / "resources" / "references" / "images" / "ARRI_Helen_John_ALEXA_Mini_LF_AWG3_LogC3.tif"
        self.input_image = PathRow("input", str(default_image))
        self.output_image = PathRow("output", str(PROJECT_ROOT / "build" / "filmviz_output.tif"))
        image_form.addRow("Input image", self.input_image)
        image_form.addRow("Output image", self.output_image)
        self.negative_grain = _double(0.0, 0.0, 10.0)
        self.print_grain = _double(0.0, 0.0, 10.0)
        self.grain_size = _double(1.0, 1.0, 32.0, 0.25)
        self.grain_chroma = _double(1.0, 0.0, 4.0, 0.1)
        self.grain_seed = QSpinBox()
        self.grain_seed.setRange(0, 2_147_483_647)
        self.grain_seed.setValue(1)
        image_form.addRow("Negative grain", self.negative_grain)
        image_form.addRow("Print grain", self.print_grain)
        image_form.addRow("Grain size (px)", self.grain_size)
        image_form.addRow("Grain chroma", self.grain_chroma)
        image_form.addRow("Grain seed", self.grain_seed)
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
        tabs.addTab(image_tab, "Convert Image")

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
        tabs.addTab(lut_tab, "Generate LUT")

        self.stage = QLabel("Ready")
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        root_layout.addWidget(self.stage)
        root_layout.addWidget(self.progress)

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
        )
        output = arguments["output_filename"]
        self._start(
            lambda progress: filmviz.process_image(**arguments, progress=progress),
            f"Wrote {output}",
            output_image=output,
        )

    @Slot()
    def generate_lut(self):
        arguments = self._common()
        arguments["output_filename"] = self.output_lut.value()
        output = arguments["output_filename"]
        self._start(
            lambda progress: filmviz.generate_lut(**arguments, progress=progress),
            f"Wrote {output}",
        )

    def _start(self, operation, success_message, output_image=None):
        if self._thread is not None:
            return
        self._pending_output_image = output_image
        if output_image is not None:
            self.open_output_button.setVisible(False)
            self.open_output_button.setEnabled(False)
        self.convert_button.setEnabled(False)
        self.lut_button.setEnabled(False)
        self.progress.setValue(0)
        self.stage.setText("Starting…")
        thread = QThread(self)
        worker = OperationWorker(operation, success_message)
        worker.moveToThread(thread)
        thread.started.connect(worker.run)
        worker.progress.connect(self._progress)
        worker.finished.connect(self._finished)
        worker.failed.connect(self._failed)
        worker.finished.connect(thread.quit)
        worker.failed.connect(thread.quit)
        thread.finished.connect(worker.deleteLater)
        thread.finished.connect(thread.deleteLater)
        thread.finished.connect(self._clear_worker)
        self._thread = thread
        self._worker = worker
        thread.start()

    @Slot(str, int, int)
    def _progress(self, stage, completed, total):
        self.stage.setText(stage)
        self.progress.setValue(round(100 * completed / total) if total else 0)

    @Slot(str)
    def _finished(self, message):
        self.progress.setValue(100)
        self.stage.setText(message)
        if self._pending_output_image is not None:
            path = Path(self._pending_output_image).expanduser().resolve()
            self._last_output_image = str(path)
            self.open_output_button.setEnabled(path.is_file())
            self.open_output_button.setVisible(True)

    @Slot(str)
    def _failed(self, message):
        self.stage.setText("Failed")
        if self._pending_output_image is not None:
            self.open_output_button.setVisible(False)
            self.open_output_button.setEnabled(False)
        QMessageBox.critical(self, "FilmViz failed", message)

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
