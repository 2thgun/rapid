import QtQuick
import QtQuick.Window

Window {
    id: root
    width: 480
    height: 320
    visible: true
    flags: Qt.FramelessWindowHint
    color: "#0b1014"

    property int page: 0
    property int revision: dashboard.revision
    readonly property var state: dashboard
    readonly property color accent: "#f6b91a"
    readonly property color muted: "#9dacb5"

    function text(key, fallback) {
        revision
        const value = dashboard.value(key)
        return value === undefined || value === null || value === "" ? (fallback || "—") : value
    }
    function number(key, fallback) {
        revision
        const value = Number(dashboard.value(key))
        return Number.isFinite(value) ? value : (fallback || 0)
    }
    function time(key) { revision; return dashboard.timeValue(key) }
    function percent(key) { revision; return dashboard.percentValue(key) }
    function clamp(value, minimum, maximum) { return Math.max(minimum, Math.min(maximum, value)) }
    function gearText() {
        revision
        const gear = dashboard.value("gear")
        if (gear === undefined || gear === null) return "—"
        if (Number(gear) === -1) return "R"
        if (Number(gear) === 0) return "N"
        return String(gear)
    }

    Rectangle { anchors.fill: parent; color: "#0b1014" }
    Rectangle { id: header; x: 8; y: 6; width: root.width - 16; height: 28; radius: 3; color: "#121a20"; border.color: "#28353d"
        Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.right: network.left; anchors.rightMargin: 7; anchors.verticalCenter: parent.verticalCenter; text: dashboard.status; color: root.muted; font.pixelSize: 10; font.bold: true; elide: Text.ElideRight }
        Row { id: network; visible: dashboard.networkAvailable; anchors.right: parent.right; anchors.rightMargin: 3; anchors.verticalCenter: parent.verticalCenter; spacing: 3
            Repeater { model: [ {mode: "home", label: "HOME"}, {mode: "ap", label: "AP"}, {mode: "off", label: "OFF"} ]
                Rectangle { required property var modelData; width: 43; height: 22; radius: 3
                    readonly property bool selected: dashboard.networkMode === modelData.mode
                    color: selected ? "#2a2210" : "#19242b"
                    border.color: selected ? root.accent : "#3b4b55"
                    border.width: selected ? 2 : 1
                    Text { anchors.centerIn: parent; text: modelData.label; color: parent.selected ? root.accent : root.muted; font.pixelSize: 9; font.bold: true }
                    MouseArea { anchors.fill: parent; onClicked: dashboard.setNetworkMode(modelData.mode) }
                }
            }
        }
    }

    Item { id: body; x: 8; y: 39; width: root.width - 16; height: 235
        Item { visible: root.page === 0; anchors.fill: parent
            Rectangle { width: 276; height: 139; radius: 4; color: "#121a20"; border.color: "#28353d"
                Text { x: 14; y: 11; text: "GEAR"; color: root.muted; font.pixelSize: 10; font.bold: true }
                Text { x: 13; y: 29; width: 72; text: root.gearText(); color: "#f4f7f9"; font.pixelSize: 55; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                Text { x: 103; y: 16; width: 150; text: Math.round(root.number("speed_kmh")); color: "#f4f7f9"; font.pixelSize: 58; font.bold: true; horizontalAlignment: Text.AlignRight }
                Text { x: 195; y: 77; width: 58; text: "KM/H"; color: root.muted; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignRight }
                Text { x: 14; y: 105; text: "RPM  " + root.text("rpm"); color: root.muted; font.pixelSize: 11; font.bold: true }
                Rectangle { x: 14; y: 125; width: 248; height: 6; color: "#28353d"; radius: 3 }
                Rectangle { x: 14; y: 125; width: 248 * root.clamp(root.number("rpm") / 10000, 0, 1); height: 6; color: root.accent; radius: 3 }
            }
            Rectangle { x: 284; width: 180; height: 139; radius: 4; color: "#121a20"; border.color: "#28353d"
                Text { x: 12; y: 12; text: "LAP  " + root.text("lap_number"); color: root.muted; font.pixelSize: 11; font.bold: true }
                Text { x: 12; y: 43; text: "CURRENT  " + root.time("current_lap_ms"); color: "#f4f7f9"; font.pixelSize: 11; font.bold: true }
                Text { x: 12; y: 73; text: "LAST       " + root.time("completed_lap_ms"); color: "#f4f7f9"; font.pixelSize: 11; font.bold: true }
                Text { x: 12; y: 103; text: dashboard.value("recording") ? "REC" : "IDLE"; color: dashboard.value("recording") ? "#20cf75" : root.muted; font.pixelSize: 15; font.bold: true }
            }
            Rectangle { y: 147; width: 300; height: 80; radius: 4; color: "#121a20"; border.color: "#28353d"
                Text { x: 12; y: 10; text: "THROTTLE  " + root.percent("throttle"); color: root.muted; font.pixelSize: 10; font.bold: true }
                Rectangle { x: 12; y: 29; width: 272; height: 10; color: "#28353d"; radius: 3 }
                Rectangle { x: 12; y: 29; width: 272 * root.clamp(root.number("throttle"), 0, 1); height: 10; color: "#20cf75"; radius: 3 }
                Text { x: 12; y: 49; text: "BRAKE       " + root.percent("brake"); color: root.muted; font.pixelSize: 10; font.bold: true }
                Rectangle { x: 12; y: 68; width: 272; height: 10; color: "#28353d"; radius: 3 }
                Rectangle { x: 12; y: 68; width: 272 * root.clamp(root.number("brake"), 0, 1); height: 10; color: "#ef4458"; radius: 3 }
            }
            Rectangle { x: 308; y: 147; width: 156; height: 80; radius: 4; color: "#121a20"; border.color: "#28353d"
                Image { anchors.centerIn: parent; width: 66; height: 66; fillMode: Image.PreserveAspectFit; source: "qrc:/assets/steering-wheel-cartoon.png"; rotation: root.number("steering_angle") * 180 / Math.PI }
                Text { x: 9; y: 8; text: "STEER"; color: root.muted; font.pixelSize: 9; font.bold: true }
            }
        }
        Item { visible: root.page === 1; anchors.fill: parent
            Repeater { model: [ ["TRACK", root.text("track_name")], ["CAR", root.text("car_model")], ["DRIVER", root.text("driver_name")], ["CURRENT", root.time("current_lap_ms")], ["BEST", root.time("best_lap_ms")], ["SAMPLES", root.text("recorded_samples")] ]
                Rectangle { required property var modelData; required property int index; x: (index % 2) * 236; y: Math.floor(index / 2) * 72; width: 228; height: 64; radius: 4; color: "#121a20"; border.color: "#28353d"
                    Text { x: 10; y: 9; text: modelData[0]; color: root.muted; font.pixelSize: 10; font.bold: true }
                    Text { x: 10; y: 29; width: 208; text: modelData[1]; color: "#f4f7f9"; font.pixelSize: 14; font.bold: true; elide: Text.ElideRight }
                }
            }
        }
        Item { visible: root.page === 2; anchors.fill: parent
            Repeater { model: [ ["FUEL", root.text("fuel") + " L"], ["TC", root.text("tc")], ["ABS", root.percent("abs_activity")], ["LIMITER", dashboard.value("pit_limiter") ? "ON" : "OFF"], ["DAMAGE FRONT", root.percent("damage_front")], ["DAMAGE REAR", root.percent("damage_rear")] ]
                Rectangle { required property var modelData; required property int index; x: (index % 2) * 236; y: Math.floor(index / 2) * 72; width: 228; height: 64; radius: 4; color: "#121a20"; border.color: "#28353d"
                    Text { x: 10; y: 9; text: modelData[0]; color: root.muted; font.pixelSize: 10; font.bold: true }
                    Text { x: 10; y: 29; text: modelData[1]; color: "#f4f7f9"; font.pixelSize: 17; font.bold: true }
                }
            }
        }
        Item { visible: root.page === 3; anchors.fill: parent
            Repeater { model: [ ["FL", root.text("core_temp_fl") + " C  " + root.text("pressure_fl")], ["FR", root.text("core_temp_fr") + " C  " + root.text("pressure_fr")], ["RL", root.text("core_temp_rl") + " C  " + root.text("pressure_rl")], ["RR", root.text("core_temp_rr") + " C  " + root.text("pressure_rr")] ]
                Rectangle { required property var modelData; required property int index; x: (index % 2) * 236; y: Math.floor(index / 2) * 72; width: 228; height: 64; radius: 4; color: "#121a20"; border.color: "#28353d"
                    Text { x: 10; y: 9; text: modelData[0]; color: root.muted; font.pixelSize: 10; font.bold: true }
                    Text { x: 10; y: 29; text: modelData[1]; color: "#f4f7f9"; font.pixelSize: 15; font.bold: true }
                }
            }
        }
        Item { visible: root.page === 4; anchors.fill: parent
            property var samples: dashboard.graphSamples
            function sampleNumber(sample, key) {
                const value = sample[key]
                return value === undefined || value === null || !Number.isFinite(Number(value)) ? null : Number(value)
            }
            function drawGraph(context, canvas, lines, minimum, maximum) {
                const now = Date.now()
                context.reset()
                context.fillStyle = "#121a20"
                context.fillRect(0, 0, canvas.width, canvas.height)
                context.strokeStyle = "#28353d"
                context.lineWidth = 1
                for (let row = 1; row < 4; ++row) {
                    const y = Math.round(canvas.height * row / 4) + 0.5
                    context.beginPath(); context.moveTo(0, y); context.lineTo(canvas.width, y); context.stroke()
                }
                for (const line of lines) {
                    context.strokeStyle = line.color; context.lineWidth = 2
                    let drawing = false
                    context.beginPath()
                    for (const sample of samples) {
                        const value = sampleNumber(sample, line.key)
                        if (value === null) { drawing = false; continue }
                        const x = canvas.width * (1 - (now - Number(sample.time)) / 30000)
                        const y = canvas.height * (1 - clamp((value - minimum) / (maximum - minimum), 0, 1))
                        if (!drawing) { context.moveTo(x, y); drawing = true } else context.lineTo(x, y)
                    }
                    context.stroke()
                }
            }
            Text { x: 10; y: 3; text: "PEDALS   T " + root.percent("throttle") + "   B " + root.percent("brake"); color: root.muted; font.pixelSize: 10; font.bold: true }
            Canvas { id: pedalGraph; x: 0; y: 20; width: parent.width; height: 83
                onPaint: root.drawGraph(getContext("2d"), pedalGraph, [{key: "throttle", color: "#20cf75"}, {key: "brake", color: "#ef4458"}], 0, 100)
                onVisibleChanged: if (visible) requestPaint()
                Component.onCompleted: requestPaint()
                Connections { target: dashboard; function onChanged() { pedalGraph.requestPaint() } }
            }
            Text { x: 10; y: 112; text: "G-FORCE   LAT " + root.number("g_x").toFixed(2) + "   LONG " + root.number("g_z").toFixed(2); color: root.muted; font.pixelSize: 10; font.bold: true }
            Canvas { id: forceGraph; x: 0; y: 129; width: parent.width; height: 83
                onPaint: root.drawGraph(getContext("2d"), forceGraph, [{key: "lateral", color: "#34bdf2"}, {key: "longitudinal", color: "#f6b91a"}], -2.5, 2.5)
                onVisibleChanged: if (visible) requestPaint()
                Component.onCompleted: requestPaint()
                Connections { target: dashboard; function onChanged() { forceGraph.requestPaint() } }
            }
        }
    }
    Rectangle { x: 8; y: 282; width: root.width - 16; height: 30; radius: 3; color: "#121a20"; border.color: "#28353d"
        Row { anchors.centerIn: parent; spacing: 5
            Repeater { model: ["DRIVE", "TIMING", "VEHICLE", "TYRES", "GRAPHS"]
                Rectangle { required property string modelData; required property int index; width: 83; height: 23; radius: 2; color: index === root.page ? "#2a2210" : "#0e151a"; border.color: index === root.page ? root.accent : "#28353d"
                    Text { anchors.centerIn: parent; text: modelData; color: index === root.page ? root.accent : root.muted; font.pixelSize: 9; font.bold: true }
                    MouseArea { anchors.fill: parent; onClicked: root.page = index }
                }
            }
        }
    }
    Rectangle { visible: dashboard.logNotice.length > 0; x: 289; y: 8; width: 82; height: 17; radius: 2; color: "#193425"; border.color: "#20cf75"; z: 2
        Text { anchors.centerIn: parent; text: dashboard.logNotice; color: "#d8f9e6"; font.pixelSize: 8; font.bold: true }
    }
}
