// Main.qml — viewer UI demonstrating the full phase 1 + phase 2 API:
//
//   - source / artboard / state-machine selection (dropdowns)
//   - state-machine input controls (auto-built from the active SM)
//   - view-model panel (auto-built from the bound VM's properties,
//     with type-specific editors per property)
//   - layout-size override (drives rive's runtime layout system)
//   - reported-event log
//   - keyboard focus indicator + Tab/arrow-key navigation
//   - mouse + touch dispatch via RiveView's built-in forwarding

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Qt.labs.folderlistmodel
import Hypernuclear.Rive

// VM property panel — extracted as a top-level Component so it can
// recurse for nested-VM properties without duplicating the dispatch.

ApplicationWindow {
    id: window
    width: 1280
    height: 820
    visible: true
    title: qsTr("Rive viewer")
    color: "#141414"

    FolderListModel {
        id: samplesModel
        folder: viewerSamplesDir
        nameFilters: ["*.riv"]
        showDirs: false
        sortField: FolderListModel.Name
    }

    // Local path → file URL. Windows local paths start with a drive
    // letter ("C:/..."); the valid file URL form is file:///C:/...,
    // NOT file://C:/... (the latter is parsed as host="C:" and
    // triggers a UNC server lookup). On Unix an absolute path starts
    // with "/", so two slashes is already correct.
    function localPathToFileUrl(path) {
        if (path.length >= 2 && path[1] === ":")
            return "file:///" + path;
        return "file://" + path;
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // ----- Top toolbar: source / artboard / SM / VM selectors --------

        GridLayout {
            Layout.fillWidth: true
            columns: 8
            columnSpacing: 8
            rowSpacing: 6

            Label { text: qsTr("Sample:"); color: "#ddd" }
            ComboBox {
                id: sampleSelector
                Layout.fillWidth: true
                Layout.columnSpan: 7
                textRole: "fileName"
                valueRole: "filePath"
                model: samplesModel
                onActivated: {
                    rive.source = localPathToFileUrl(currentValue)
                    artboardSelector.currentIndex = 0
                    stateMachineSelector.currentIndex = 0
                    viewModelSelector.currentIndex = 0
                    viewModelInstanceSelector.currentIndex = 0
                    rive.artboard = ""
                    rive.stateMachineName = ""
                    rive.viewModelName = ""
                    rive.viewModelInstanceName = ""
                }
            }

            Label { text: qsTr("Artboard:"); color: "#ddd" }
            ComboBox {
                id: artboardSelector
                Layout.preferredWidth: 180
                model: [qsTr("(default)")].concat(rive.artboardNames)
                onActivated: {
                    rive.artboard = (currentIndex === 0 ? "" : currentText)
                    stateMachineSelector.currentIndex = 0
                    rive.stateMachineName = ""
                }
            }

            Label { text: qsTr("State machine:"); color: "#ddd" }
            ComboBox {
                id: stateMachineSelector
                Layout.preferredWidth: 180
                model: [qsTr("(default)")].concat(rive.stateMachineNames)
                onActivated: rive.stateMachineName =
                    (currentIndex === 0 ? "" : currentText)
            }

            Label { text: qsTr("View model:"); color: "#ddd" }
            ComboBox {
                id: viewModelSelector
                Layout.preferredWidth: 160
                model: [qsTr("(artboard default)")].concat(rive.viewModelNames)
                onActivated: {
                    rive.viewModelName = (currentIndex === 0 ? "" : currentText)
                    viewModelInstanceSelector.currentIndex = 0
                    rive.viewModelInstanceName = ""
                }
            }

            Label { text: qsTr("Instance:"); color: "#ddd" }
            ComboBox {
                id: viewModelInstanceSelector
                Layout.preferredWidth: 140
                // Instance presets aren't currently surfaced by RiveView
                // (would require exposing RiveViewModel.instanceNames).
                // For now this is a free-text equivalent via TextField.
                editable: true
                model: [qsTr("(blank)")]
                onAccepted: rive.viewModelInstanceName =
                    editText === "(blank)" ? "" : editText
            }

            Button {
                text: rive.playing ? qsTr("Pause") : qsTr("Play")
                onClicked: rive.playing = !rive.playing
            }
        }

        // ----- Main row: rive view + side panel --------------------------

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // Rive surface.
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 320
                color: "#0a0a0a"
                border.color: rive.activeFocus ? "#4a8af4" : "#2a2a2a"
                border.width: 1

                RiveView {
                    id: rive
                    anchors.fill: parent
                    anchors.margins: 12
                    fit: RiveView.Fit.Contain
                    focus: true

                    onLoadFailed: reason => {
                        status.text = qsTr("Failed: ") + reason
                    }

                    onEventReported: event => {
                        eventLog.append(
                            Qt.formatTime(new Date(), "hh:mm:ss") +
                            "  " + event.name +
                            (event.delay > 0 ? " (+" + event.delay.toFixed(2) + "s)" : ""))
                    }
                }
            }

            // Side panel: SM inputs + VM properties + event log + layout.
            Rectangle {
                Layout.preferredWidth: 320
                Layout.maximumWidth: 320
                Layout.fillHeight: true
                color: "#0d0d0d"
                border.color: "#222"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 6

                    TabBar {
                        id: sideTabBar
                        Layout.fillWidth: true
                        TabButton { text: qsTr("View Model") }
                        TabButton { text: qsTr("Layout") }
                        TabButton { text: qsTr("Events") }
                    }

                    StackLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: sideTabBar.currentIndex

                        // ----- View Model tab --------------------------------

                        ScrollView {
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                            ColumnLayout {
                                width: parent.width
                                spacing: 6

                                // Header — shows which VM is bound.
                                // Helpful when switching .riv files so
                                // you can confirm the panel did refresh.
                                Label {
                                    Layout.fillWidth: true
                                    text: rive.viewModel
                                              ? qsTr("Bound: ") +
                                                (rive.viewModelName || qsTr("(artboard default)")) +
                                                "  ·  " +
                                                rive.viewModel.propertyNames.length +
                                                qsTr(" property/properties")
                                              : qsTr("(no view model bound)")
                                    color: rive.viewModel ? "#aaa" : "#777"
                                    font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                }

                                Loader {
                                    Layout.fillWidth: true
                                    sourceComponent: vmPanelComponent
                                    property var vmInstance: rive.viewModel
                                }
                            }
                        }

                        // ----- Layout tab ------------------------------------

                        ColumnLayout {
                            spacing: 8

                            CheckBox {
                                id: layoutOverrideEnable
                                text: qsTr("Override artboard layout size")
                                onCheckedChanged: {
                                    if (checked)
                                        rive.layoutSize = Qt.size(layoutW.value, layoutH.value)
                                    else
                                        rive.layoutSize = Qt.size(-1, -1)
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                enabled: layoutOverrideEnable.checked
                                Label { text: qsTr("Width:"); color: "#ddd" }
                                SpinBox {
                                    id: layoutW
                                    from: 50
                                    to: 4096
                                    value: 500
                                    Layout.fillWidth: true
                                    onValueModified: rive.layoutSize = Qt.size(value, layoutH.value)
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                enabled: layoutOverrideEnable.checked
                                Label { text: qsTr("Height:"); color: "#ddd" }
                                SpinBox {
                                    id: layoutH
                                    from: 50
                                    to: 4096
                                    value: 500
                                    Layout.fillWidth: true
                                    onValueModified: rive.layoutSize = Qt.size(layoutW.value, value)
                                }
                            }

                            Label {
                                text: qsTr("Visible only when the artboard uses rive's layout system.")
                                color: "#666"
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Item { Layout.fillHeight: true }
                        }

                        // ----- Events tab ------------------------------------

                        ColumnLayout {
                            spacing: 6

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                color: "#070707"
                                border.color: "#222"
                                border.width: 1

                                ScrollView {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    TextArea {
                                        id: eventLog
                                        readOnly: true
                                        color: "#b0e0b0"
                                        font.family: "Menlo"
                                        font.pixelSize: 11
                                        wrapMode: TextArea.Wrap
                                        placeholderText: qsTr("(events appear here)")
                                    }
                                }
                            }

                            Label {
                                text: qsTr("Keyboard focus: ") +
                                      (rive.activeFocus ? qsTr("yes") : qsTr("no"))
                                color: rive.activeFocus ? "#8af08a" : "#888"
                                font.pixelSize: 11
                            }

                            Label {
                                text: qsTr("Click into the view to grab focus. Tab / arrows navigate inside the artboard.")
                                color: "#666"
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                font.pixelSize: 11
                            }
                        }
                    }
                }
            }
        }

        Label {
            id: status
            Layout.fillWidth: true
            text: rive.source.toString().split("/").pop() || qsTr("(no source)")
            color: "#888"
            elide: Text.ElideRight
        }
    }

    // ----- Reusable VM panel ---------------------------------------------
    //
    // Renders a vertical list of property rows for the VM instance
    // bound to the parent Loader's `vmInstance` property. Used by the
    // top-level View Model tab and recursively by the nested-VM
    // delegate, which keeps the dispatch logic in one place.

    Component {
        id: vmPanelComponent
        ColumnLayout {
            spacing: 6
            // The Loader that hosts this Component sets `vmInstance`.
            // We read it back through parent (Loader).
            property var vmInstance: parent ? parent.vmInstance : null

            Label {
                Layout.fillWidth: true
                visible: !vmInstance ||
                         vmInstance.propertyNames.length === 0
                text: vmInstance ? qsTr("(view model has no properties)")
                                 : qsTr("(no view model bound)")
                color: "#777"
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: vmInstance ? vmInstance.propertyNames : []

                delegate: Loader {
                    Layout.fillWidth: true
                    property string propName: modelData
                    property var prop: vmInstance
                                ? vmInstance.property(propName)
                                : null
                    sourceComponent: {
                        if (!prop) return unsupportedDelegate
                        switch (prop.type) {
                        case RiveVMProperty.Number:    return numberDelegate
                        case RiveVMProperty.Boolean:   return booleanDelegate
                        case RiveVMProperty.String:    return stringDelegate
                        case RiveVMProperty.Color:     return colorDelegate
                        case RiveVMProperty.Enum:      return enumDelegate
                        case RiveVMProperty.Trigger:   return triggerDelegate
                        case RiveVMProperty.ViewModel: return nestedVMDelegate
                        case RiveVMProperty.List:      return listDelegate
                        case RiveVMProperty.Image:     return imageDelegate
                        case RiveVMProperty.Artboard:  return artboardDelegate
                        }
                        return unsupportedDelegate
                    }
                }
            }
        }
    }

    // ----- VM property delegate components --------------------------------

    Component {
        id: numberDelegate
        RowLayout {
            spacing: 6
            Label {
                text: prop ? prop.name : ""
                color: "#ddd"
                Layout.preferredWidth: 90
                elide: Text.ElideRight
            }
            Slider {
                Layout.fillWidth: true
                from: 0
                to: 100
                value: prop ? prop.value : 0
                onMoved: if (prop) prop.value = value
            }
            Label {
                text: prop ? prop.value.toFixed(1) : ""
                color: "#888"
                font.family: "Menlo"
                font.pixelSize: 11
                Layout.preferredWidth: 36
            }
        }
    }

    Component {
        id: booleanDelegate
        RowLayout {
            spacing: 6
            Label {
                text: prop ? prop.name : ""
                color: "#ddd"
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Switch {
                checked: prop ? prop.value : false
                onClicked: if (prop) prop.value = checked
            }
        }
    }

    Component {
        id: stringDelegate
        RowLayout {
            spacing: 6
            Label {
                text: prop ? prop.name : ""
                color: "#ddd"
                Layout.preferredWidth: 90
                elide: Text.ElideRight
            }
            TextField {
                Layout.fillWidth: true
                text: prop ? prop.value : ""
                onEditingFinished: if (prop) prop.value = text
            }
        }
    }

    Component {
        id: colorDelegate
        RowLayout {
            spacing: 6
            Label {
                text: prop ? prop.name : ""
                color: "#ddd"
                Layout.preferredWidth: 90
                elide: Text.ElideRight
            }
            Rectangle {
                Layout.preferredWidth: 32
                Layout.preferredHeight: 24
                radius: 4
                color: prop ? prop.value : "#000"
                border.color: "#444"
                border.width: 1
                MouseArea {
                    anchors.fill: parent
                    onClicked: colorDialog.open()
                }
                ColorDialog {
                    id: colorDialog
                    selectedColor: prop ? prop.value : "#000"
                    onAccepted: if (prop) prop.value = selectedColor
                }
            }
            Label {
                text: prop ? prop.value : ""
                color: "#888"
                font.family: "Menlo"
                font.pixelSize: 10
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }
    }

    Component {
        id: enumDelegate
        RowLayout {
            spacing: 6
            Label {
                text: prop ? prop.name : ""
                color: "#ddd"
                Layout.preferredWidth: 90
                elide: Text.ElideRight
            }
            ComboBox {
                Layout.fillWidth: true
                model: prop ? prop.values : []
                currentIndex: prop ? prop.valueIndex : 0
                onActivated: if (prop) prop.valueIndex = currentIndex
            }
        }
    }

    // Fallback for properties whose type isn't covered by any of the
    // typed delegates. Should be rare now that we cover all of rive's
    // public VM types — symbol-list-index is the only one missing.
    Component {
        id: unsupportedDelegate
        RowLayout {
            spacing: 6
            Label {
                text: propName
                color: "#888"
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Label {
                text: qsTr("(unsupported type)")
                color: "#555"
                font.pixelSize: 10
                font.italic: true
            }
        }
    }

    // Nested ViewModel: render the nested VM's panel inside an indented,
    // collapsible block. Recursive — vmPanelComponent dispatches the
    // same way for whatever the nested VM contains.
    Component {
        id: nestedVMDelegate
        ColumnLayout {
            spacing: 4
            property bool expanded: true

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Button {
                    flat: true
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    text: expanded ? "▾" : "▸"
                    onClicked: expanded = !expanded
                }
                Label {
                    text: prop ? prop.name : ""
                    color: "#ddd"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Label {
                    text: qsTr("VM")
                    color: "#666"
                    font.pixelSize: 10
                    font.italic: true
                }
            }

            Loader {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                visible: expanded && active
                active: expanded
                sourceComponent: vmPanelComponent
                property var vmInstance: prop ? prop.value : null
            }
        }
    }

    // List of nested VM instances — header with count + add/clear,
    // then a Repeater of indented child panels each with a remove
    // button.
    Component {
        id: listDelegate
        ColumnLayout {
            spacing: 4
            property bool expanded: true

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Button {
                    flat: true
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    text: expanded ? "▾" : "▸"
                    onClicked: expanded = !expanded
                }
                Label {
                    text: prop ? prop.name : ""
                    color: "#ddd"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Label {
                    text: prop ? "[" + prop.count + "]" : ""
                    color: "#888"
                    font.pixelSize: 11
                    font.family: "Menlo"
                }
                Button {
                    text: qsTr("Clear")
                    enabled: prop && prop.count > 0
                    onClicked: if (prop) prop.clear()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                visible: expanded
                spacing: 6

                Repeater {
                    model: prop ? prop.count : 0

                    delegate: ColumnLayout {
                        property int itemIndex: index
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: "[" + itemIndex + "]"
                                color: "#888"
                                font.family: "Menlo"
                                font.pixelSize: 11
                            }
                            Item { Layout.fillWidth: true }
                            Button {
                                text: qsTr("✕")
                                Layout.preferredWidth: 24
                                Layout.preferredHeight: 22
                                onClicked: if (prop) prop.removeAt(itemIndex)
                            }
                        }

                        Loader {
                            Layout.fillWidth: true
                            Layout.leftMargin: 8
                            sourceComponent: vmPanelComponent
                            property var vmInstance: prop ? prop.itemAt(itemIndex) : null
                        }
                    }
                }
            }
        }
    }

    // Image asset: button opens a file picker; chosen file is decoded
    // via QImage and pushed to rive. No preview/getter today.
    Component {
        id: imageDelegate
        RowLayout {
            spacing: 6
            Label {
                text: prop ? prop.name : ""
                color: "#ddd"
                Layout.preferredWidth: 90
                elide: Text.ElideRight
            }
            Label {
                text: qsTr("(image)")
                color: "#666"
                font.italic: true
                font.pixelSize: 10
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Choose...")
                onClicked: imageFileDialog.open()
                FileDialog {
                    id: imageFileDialog
                    nameFilters: [ "Images (*.png *.jpg *.jpeg *.webp)" ]
                    onAccepted: if (prop) prop.setSource(selectedFile)
                }
            }
        }
    }

    // Artboard reference: dropdown of all artboards in the file. The
    // current value is the artboard name (string).
    Component {
        id: artboardDelegate
        RowLayout {
            spacing: 6
            Label {
                text: prop ? prop.name : ""
                color: "#ddd"
                Layout.preferredWidth: 90
                elide: Text.ElideRight
            }
            ComboBox {
                Layout.fillWidth: true
                model: [qsTr("(none)")].concat(rive.artboardNames)
                currentIndex: {
                    if (!prop || !prop.value) return 0
                    const idx = rive.artboardNames.indexOf(prop.value)
                    return idx >= 0 ? idx + 1 : 0
                }
                onActivated: if (prop) prop.value = (currentIndex === 0 ? "" : currentText)
            }
        }
    }

    Component {
        id: triggerDelegate
        RowLayout {
            spacing: 6
            // Brief flash so you can see when the trigger fires —
            // both from our own button press and from rive-internal
            // sources (e.g. an SM transition firing the trigger).
            property bool flash: false
            Connections {
                target: prop
                function onTriggered() {
                    flash = true
                    flashTimer.restart()
                }
            }
            Timer {
                id: flashTimer
                interval: 250
                onTriggered: flash = false
            }
            Rectangle {
                Layout.preferredWidth: 10
                Layout.preferredHeight: 10
                radius: 5
                color: flash ? "#ffcc40" : "#333"
                Behavior on color { ColorAnimation { duration: 150 } }
            }
            Label {
                text: prop ? prop.name : ""
                color: "#ddd"
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Button {
                text: qsTr("Fire")
                onClicked: if (prop) prop.fire()
            }
        }
    }

    Connections {
        target: samplesModel
        function onCountChanged() {
            if (samplesModel.count > 0 && !rive.source.toString().length) {
                // Prefer a sample known to exercise view-model bindings
                // so the demo lands on something testable. Falls
                // through to the first sample if none of the preferred
                // ones are present.
                const preferred = [
                    "data_binding_test.riv",
                    "quick_start_health_bar.riv",
                    "db_list_test.riv",
                    "coffee_loader.riv"
                ];
                let pickedIdx = 0;
                for (const name of preferred) {
                    const idx = [...Array(samplesModel.count).keys()].findIndex(
                        i => samplesModel.get(i, "fileName") === name);
                    if (idx >= 0) { pickedIdx = idx; break; }
                }
                sampleSelector.currentIndex = pickedIdx;
                rive.source = localPathToFileUrl(
                    samplesModel.get(pickedIdx, "filePath"));
            }
        }
    }
}
