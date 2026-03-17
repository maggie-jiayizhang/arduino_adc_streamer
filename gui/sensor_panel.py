"""
Sensor Panel GUI Component
==========================
Provides UI for selecting and editing named 5-channel sensor layouts.
"""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QComboBox,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from sensor_config import (
    SENSOR_POSITION_LABELS,
    SENSOR_POSITION_ORDER,
    SensorConfigStore,
    default_sensor_configuration,
    mapping_to_position_channels,
    normalize_sensor_config,
    position_channels_to_mapping,
)


class SensorPanelMixin:
    """Mixin providing a Sensor tab for managing named sensor mappings."""

    def init_sensor_config_state(self):
        self.sensor_config_store = SensorConfigStore()
        self.sensor_configurations = []
        self.active_sensor_config_name = ""
        self._sensor_config_ui_loading = False
        self._load_sensor_configs_from_disk()

    def _load_sensor_configs_from_disk(self):
        configs, selected_name = self.sensor_config_store.load()
        self.sensor_configurations = configs
        self.active_sensor_config_name = selected_name

    def save_sensor_configurations(self, log_message=False):
        self.sensor_config_store.save(self.sensor_configurations, self.active_sensor_config_name)
        if log_message:
            self.log_status(f"Saved sensor configurations ({len(self.sensor_configurations)})")

    def get_active_sensor_configuration(self):
        for config in self.sensor_configurations:
            if config["name"] == self.active_sensor_config_name:
                return config
        fallback = default_sensor_configuration()
        return normalize_sensor_config(fallback) or fallback

    def get_active_channel_sensor_map(self):
        return list(self.get_active_sensor_configuration()["channel_sensor_map"])

    def create_sensor_tab(self):
        sensor_widget = QWidget()
        layout = QVBoxLayout()
        layout.setContentsMargins(6, 6, 6, 6)
        layout.setSpacing(8)

        selector_group = QGroupBox("Active Sensor Configuration")
        selector_layout = QVBoxLayout()

        form_layout = QFormLayout()
        self.sensor_config_combo = QComboBox()
        self.sensor_config_combo.currentIndexChanged.connect(self.on_sensor_config_selected)
        form_layout.addRow("Sensor:", self.sensor_config_combo)

        self.sensor_name_edit = QLineEdit()
        self.sensor_name_edit.editingFinished.connect(self.on_sensor_name_edited)
        form_layout.addRow("Name:", self.sensor_name_edit)
        selector_layout.addLayout(form_layout)

        actions_layout = QHBoxLayout()
        self.sensor_add_btn = QPushButton("Add New")
        self.sensor_add_btn.clicked.connect(self.on_add_sensor_config_clicked)
        actions_layout.addWidget(self.sensor_add_btn)

        self.sensor_delete_btn = QPushButton("Delete")
        self.sensor_delete_btn.clicked.connect(self.on_delete_sensor_config_clicked)
        actions_layout.addWidget(self.sensor_delete_btn)
        actions_layout.addStretch()
        selector_layout.addLayout(actions_layout)

        selector_group.setLayout(selector_layout)
        layout.addWidget(selector_group)

        editor_group = QGroupBox("Channel Layout")
        editor_layout = QVBoxLayout()
        editor_layout.addWidget(QLabel("Set which channel number (1-5) is at each sensor position."))

        grid = QGridLayout()
        grid.setHorizontalSpacing(20)
        grid.setVerticalSpacing(10)

        self.sensor_position_spins = {}
        positions = {
            "T": (0, 1),
            "L": (1, 0),
            "C": (1, 1),
            "R": (1, 2),
            "B": (2, 1),
        }
        for sensor_label, (row, col) in positions.items():
            cell = QWidget()
            cell_layout = QVBoxLayout()
            cell_layout.setContentsMargins(0, 0, 0, 0)
            cell_layout.setSpacing(4)

            title = QLabel(SENSOR_POSITION_LABELS[sensor_label])
            title.setAlignment(Qt.AlignmentFlag.AlignCenter)
            cell_layout.addWidget(title)

            spin = QSpinBox()
            spin.setRange(1, 5)
            spin.setAlignment(Qt.AlignmentFlag.AlignCenter)
            spin.setProperty("sensor_label", sensor_label)
            spin.setProperty("previous_value", 1)
            spin.valueChanged.connect(self.on_sensor_position_spin_changed)
            self.sensor_position_spins[sensor_label] = spin
            cell_layout.addWidget(spin)

            cell.setLayout(cell_layout)
            grid.addWidget(cell, row, col)

        editor_layout.addLayout(grid)

        self.sensor_mapping_preview_label = QLabel("")
        self.sensor_mapping_preview_label.setStyleSheet("font-family: monospace;")
        editor_layout.addWidget(self.sensor_mapping_preview_label)

        editor_group.setLayout(editor_layout)
        layout.addWidget(editor_group)

        self.sensor_status_label = QLabel("")
        self.sensor_status_label.setStyleSheet("color: red; font-weight: bold;")
        layout.addWidget(self.sensor_status_label)
        layout.addStretch()

        sensor_widget.setLayout(layout)
        self._refresh_sensor_tab_ui()
        return sensor_widget

    def _refresh_sensor_tab_ui(self):
        if not hasattr(self, "sensor_config_combo"):
            return

        self._sensor_config_ui_loading = True
        self.sensor_config_combo.blockSignals(True)
        self.sensor_config_combo.clear()
        for config in self.sensor_configurations:
            self.sensor_config_combo.addItem(str(config["name"]))

        current_index = max(
            0,
            self.sensor_config_combo.findText(self.active_sensor_config_name),
        )
        self.sensor_config_combo.setCurrentIndex(current_index)
        self.sensor_config_combo.blockSignals(False)

        self._load_active_sensor_into_editor()
        self._sensor_config_ui_loading = False

    def _load_active_sensor_into_editor(self):
        if not hasattr(self, "sensor_name_edit"):
            return

        config = self.get_active_sensor_configuration()
        self.sensor_name_edit.setText(str(config["name"]))
        position_channels = mapping_to_position_channels(list(config["channel_sensor_map"]))

        for sensor_label in SENSOR_POSITION_ORDER:
            spin = self.sensor_position_spins[sensor_label]
            spin.blockSignals(True)
            spin.setValue(int(position_channels[sensor_label]))
            spin.setProperty("previous_value", int(position_channels[sensor_label]))
            spin.blockSignals(False)

        self._update_sensor_mapping_preview()
        self.refresh_sensor_mapping_usage()

    def _update_sensor_mapping_preview(self):
        mapping = self.get_active_channel_sensor_map()
        self.sensor_mapping_preview_label.setText(
            "Channel map [1..5]: " + ", ".join(f"{index + 1}->{label}" for index, label in enumerate(mapping))
        )

    def _set_active_sensor_config_name(self, name):
        self.active_sensor_config_name = str(name)

    def _replace_active_sensor_config(self, updated_config):
        for index, config in enumerate(self.sensor_configurations):
            if config["name"] == self.active_sensor_config_name:
                self.sensor_configurations[index] = updated_config
                self.active_sensor_config_name = str(updated_config["name"])
                return

        self.sensor_configurations.append(updated_config)
        self.active_sensor_config_name = str(updated_config["name"])

    def on_sensor_config_selected(self, index):
        if self._sensor_config_ui_loading or index < 0 or index >= len(self.sensor_configurations):
            return

        self._set_active_sensor_config_name(self.sensor_configurations[index]["name"])
        self._load_active_sensor_into_editor()
        self.save_sensor_configurations()
        self.log_status(f"Selected sensor configuration: {self.active_sensor_config_name}")

    def on_sensor_name_edited(self):
        if self._sensor_config_ui_loading:
            return

        new_name = self.sensor_name_edit.text().strip()
        if not new_name:
            self.sensor_status_label.setText("Sensor name cannot be empty.")
            self.sensor_name_edit.setText(self.active_sensor_config_name)
            return

        if new_name != self.active_sensor_config_name and any(
            config["name"] == new_name for config in self.sensor_configurations
        ):
            self.sensor_status_label.setText(f'A sensor named "{new_name}" already exists.')
            self.sensor_name_edit.setText(self.active_sensor_config_name)
            return

        config = self.get_active_sensor_configuration()
        updated_config = {
            "name": new_name,
            "channel_sensor_map": list(config["channel_sensor_map"]),
            "is_bundled": False,
        }
        self._replace_active_sensor_config(updated_config)
        self.sensor_status_label.setText("")
        self._refresh_sensor_tab_ui()
        self.save_sensor_configurations()
        self.log_status(f"Renamed sensor configuration to: {new_name}")

    def _current_position_channels(self):
        return {
            sensor_label: self.sensor_position_spins[sensor_label].value()
            for sensor_label in SENSOR_POSITION_ORDER
        }

    def _save_sensor_mapping_from_editor(self):
        updated_config = {
            "name": self.active_sensor_config_name,
            "channel_sensor_map": position_channels_to_mapping(self._current_position_channels()),
            "is_bundled": False,
        }
        self._replace_active_sensor_config(updated_config)
        self.sensor_status_label.setText("")
        self._update_sensor_mapping_preview()
        self.save_sensor_configurations()
        self.refresh_sensor_mapping_usage()
        self.log_status(f"Updated sensor mapping: {self.active_sensor_config_name}")

    def on_sensor_position_spin_changed(self, new_value):
        if self._sensor_config_ui_loading:
            return

        spin = self.sender()
        if spin is None:
            return

        old_value = int(spin.property("previous_value") or new_value)
        if new_value != old_value:
            for other_spin in self.sensor_position_spins.values():
                if other_spin is spin:
                    continue
                if other_spin.value() == new_value:
                    other_spin.blockSignals(True)
                    other_spin.setValue(old_value)
                    other_spin.setProperty("previous_value", old_value)
                    other_spin.blockSignals(False)
                    break

        spin.setProperty("previous_value", int(new_value))
        self._save_sensor_mapping_from_editor()

    def on_add_sensor_config_clicked(self):
        existing_names = {config["name"] for config in self.sensor_configurations}
        base_name = "New Sensor"
        suffix = 1
        new_name = base_name
        while new_name in existing_names:
            suffix += 1
            new_name = f"{base_name} {suffix}"

        new_config = {
            "name": new_name,
            "channel_sensor_map": list(self.get_active_channel_sensor_map()),
            "is_bundled": False,
        }
        self.sensor_configurations.append(new_config)
        self.active_sensor_config_name = new_name
        self.sensor_status_label.setText("")
        self._refresh_sensor_tab_ui()
        self.save_sensor_configurations()
        self.log_status(f"Added sensor configuration: {new_name}")

    def on_delete_sensor_config_clicked(self):
        if len(self.sensor_configurations) <= 1:
            self.sensor_status_label.setText("At least one sensor configuration must remain.")
            return

        name = self.active_sensor_config_name
        answer = QMessageBox.question(
            self,
            "Delete Sensor Configuration",
            f'Delete sensor configuration "{name}"?',
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return

        self.sensor_configurations = [
            config for config in self.sensor_configurations if config["name"] != name
        ]
        self.active_sensor_config_name = str(self.sensor_configurations[0]["name"])
        self.sensor_status_label.setText("")
        self._refresh_sensor_tab_ui()
        self.save_sensor_configurations()
        self.log_status(f"Deleted sensor configuration: {name}")

    def refresh_sensor_mapping_usage(self):
        if hasattr(self, "smoothed_cop_x"):
            if isinstance(self.smoothed_cop_x, list):
                self.smoothed_cop_x = [0.0 for _ in self.smoothed_cop_x]
                self.smoothed_cop_y = [0.0 for _ in self.smoothed_cop_y]
                self.smoothed_intensity = [0.0 for _ in self.smoothed_intensity]
            else:
                self.smoothed_cop_x = 0.0
                self.smoothed_cop_y = 0.0
                self.smoothed_intensity = 0.0
        for processor in getattr(self, "heatmap_signal_processors", []):
            processor.reset()
        if hasattr(self, "reset_shear_processing_state"):
            self.reset_shear_processing_state()
        if hasattr(self, "_refresh_heatmap_background_overlay"):
            self._refresh_heatmap_background_overlay(force=True)
        if hasattr(self, "refresh_shear_background_overlay"):
            self.refresh_shear_background_overlay()
