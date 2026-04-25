// Main.qml — viewer UI.
//
// Lists every .riv under samples/ (via Qt.labs.folderlistmodel reading
// RIVE_VIEWER_SAMPLES_DIR at runtime) and demonstrates the phase 1 API:
//
//   - source swap + artboard selection
//   - state-machine selection by name
//   - pointer forwarding (mouse over / click into the rive surface)
//   - reported-event log on the side
//   - keyboard + focus navigation (click into the view to give it focus,
//     then Tab / arrows move inside the artboard's focus tree)

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.folderlistmodel
import Hypernuclear.Rive

ApplicationWindow {
    id: window
    width: 1100
    height: 750
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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label { text: qsTr("Sample:"); color: "#ddd" }

            ComboBox {
                id: sampleSelector
                Layout.fillWidth: true
                textRole: "fileName"
                valueRole: "filePath"
                model: samplesModel
                onActivated: {
                    rive.source = "file://" + currentValue
                    // Reset per-source selectors to defaults.
                    artboardSelector.currentIndex = 0
                    stateMachineField.text = ""
                }
            }

            Label { text: qsTr("Artboard:"); color: "#ddd" }

            ComboBox {
                id: artboardSelector
                Layout.preferredWidth: 180
                // First entry is "(default)" (maps to the .riv's default
                // artboard when left selected); the rest come from the
                // loaded file.
                model: {
                    const base = [qsTr("(default)")]
                    return base.concat(rive.artboardNames)
                }
                onActivated: rive.artboard = (currentIndex === 0 ? "" : currentText)
            }

            Label { text: qsTr("SM:"); color: "#ddd" }

            TextField {
                id: stateMachineField
                Layout.preferredWidth: 140
                placeholderText: qsTr("default")
                onEditingFinished: rive.stateMachineName = text
            }

            Button {
                text: rive.playing ? qsTr("Pause") : qsTr("Play")
                onClicked: rive.playing = !rive.playing
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 300
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

            ColumnLayout {
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                Layout.fillHeight: true
                spacing: 6

                Label {
                    text: qsTr("Event log")
                    color: "#ddd"
                    font.bold: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#0a0a0a"
                    border.color: "#2a2a2a"
                    border.width: 1

                    ScrollView {
                        id: eventScroll
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
                    text: qsTr("Tab / arrows navigate inside the artboard. Click the view to grab focus.")
                    color: "#666"
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    font.pixelSize: 11
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

    Connections {
        target: samplesModel
        function onCountChanged() {
            if (samplesModel.count > 0 && !rive.source.toString().length) {
                const defaultIdx = Math.max(0,
                    [...Array(samplesModel.count).keys()].findIndex(
                        i => samplesModel.get(i, "fileName") === "coffee_loader.riv"
                    ));
                sampleSelector.currentIndex = defaultIdx;
                rive.source = "file://" + samplesModel.get(defaultIdx, "filePath");
            }
        }
    }
}
