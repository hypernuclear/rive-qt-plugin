// Main.qml — viewer UI.
//
// Lists every .riv under samples/ via Qt.labs.folderlistmodel (reads the
// on-disk samples dir via RIVE_VIEWER_SAMPLES_DIR passed in at compile
// time) and swaps the current selection into a RiveView.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.folderlistmodel
import Hypernuclear.Rive

ApplicationWindow {
    id: window
    width: 900
    height: 900
    visible: true
    title: qsTr("Rive viewer")
    color: "#141414"

    // viewerSamplesDir is a file:// QUrl injected by main.cpp at startup,
    // pointing at the on-disk examples/viewer/samples/ directory. Reading
    // from disk (rather than bundling via qrc) lets the user drop new
    // .riv files into that dir and see them immediately on next launch.
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
                onActivated: rive.source = "file://" + currentValue
            }

            Button {
                text: rive.playing ? qsTr("Pause") : qsTr("Play")
                onClicked: rive.playing = !rive.playing
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#0a0a0a"
            border.color: "#2a2a2a"
            border.width: 1

            RiveView {
                id: rive
                anchors.fill: parent
                anchors.margins: 12
                fit: RiveView.Fit.Contain

                MouseArea {
                    anchors.fill: parent
                    onClicked: rive.playing = !rive.playing
                }

                onLoadFailed: reason => status.text = qsTr("Failed: ") + reason
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

    // Pick the first sample once the model has populated.
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
