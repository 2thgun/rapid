import QtQuick

Rectangle {
    id: root
    width: 480
    height: 320
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
    Rectangle { id: header; x: 8; y: 7; width: root.width - 16; height: 25; radius: 3; color: "#121a20"; border.color: "#28353d"
        Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; text: dashboard.status; color: root.muted; font.pixelSize: 10; font.bold: true }
        Text { anchors.right: network.left; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; text: root.text("simulator", "raPId"); color: "#e8f0f3"; font.pixelSize: 11; font.bold: true }
        Rectangle { id: network; visible: dashboard.networkAvailable; anchors.right: parent.right; anchors.rightMargin: 4; anchors.verticalCenter: parent.verticalCenter; height: 18; width: 62; radius: 2; color: "#19242b"
            Text { anchors.centerIn: parent; text: dashboard.networkMode === "ap" ? "WIFI AP" : "WIFI HOME"; color: root.accent; font.pixelSize: 9; font.bold: true }
            MouseArea { anchors.fill: parent; onClicked: dashboard.switchNetwork() }
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
                Rectangle { required property var modelData; x: (index % 2) * 236; y: Math.floor(index / 2) * 72; width: 228; height: 64; radius: 4; color: "#121a20"; border.color: "#28353d"
                    Text { x: 10; y: 9; text: modelData[0]; color: root.muted; font.pixelSize: 10; font.bold: true }
                    Text { x: 10; y: 29; width: 208; text: modelData[1]; color: "#f4f7f9"; font.pixelSize: 14; font.bold: true; elide: Text.ElideRight }
                }
            }
        }
        Item { visible: root.page === 2; anchors.fill: parent
            Repeater { model: [ ["FUEL", root.text("fuel") + " L"], ["TC", root.text("tc")], ["ABS", root.percent("abs_activity")], ["LIMITER", dashboard.value("pit_limiter") ? "ON" : "OFF"], ["DAMAGE FRONT", root.percent("damage_front")], ["DAMAGE REAR", root.percent("damage_rear")] ]
                Rectangle { required property var modelData; x: (index % 2) * 236; y: Math.floor(index / 2) * 72; width: 228; height: 64; radius: 4; color: "#121a20"; border.color: "#28353d"
                    Text { x: 10; y: 9; text: modelData[0]; color: root.muted; font.pixelSize: 10; font.bold: true }
                    Text { x: 10; y: 29; text: modelData[1]; color: "#f4f7f9"; font.pixelSize: 17; font.bold: true }
                }
            }
        }
        Item { visible: root.page === 3; anchors.fill: parent
            Repeater { model: [ ["FL", root.text("core_temp_fl") + " C  " + root.text("pressure_fl")], ["FR", root.text("core_temp_fr") + " C  " + root.text("pressure_fr")], ["RL", root.text("core_temp_rl") + " C  " + root.text("pressure_rl")], ["RR", root.text("core_temp_rr") + " C  " + root.text("pressure_rr")] ]
                Rectangle { required property var modelData; x: (index % 2) * 236; y: Math.floor(index / 2) * 72; width: 228; height: 64; radius: 4; color: "#121a20"; border.color: "#28353d"
                    Text { x: 10; y: 9; text: modelData[0]; color: root.muted; font.pixelSize: 10; font.bold: true }
                    Text { x: 10; y: 29; text: modelData[1]; color: "#f4f7f9"; font.pixelSize: 15; font.bold: true }
                }
            }
        }
        Item { visible: root.page === 4; anchors.fill: parent
            Text { anchors.horizontalCenter: parent.horizontalCenter; y: 22; text: "GRAPHS — NEXT QT SLICE"; color: root.accent; font.pixelSize: 16; font.bold: true }
            Text { anchors.horizontalCenter: parent.horizontalCenter; y: 58; width: 380; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; text: "The existing browser dashboard remains the graph reference while Qt history rendering is implemented."; color: root.muted; font.pixelSize: 12 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; y: 132; text: "THROTTLE " + root.percent("throttle") + "    BRAKE " + root.percent("brake"); color: "#f4f7f9"; font.pixelSize: 15; font.bold: true }
            Text { anchors.horizontalCenter: parent.horizontalCenter; y: 165; text: "Gx " + root.number("g_x").toFixed(2) + "    Gz " + root.number("g_z").toFixed(2); color: "#f4f7f9"; font.pixelSize: 15; font.bold: true }
        }
    }
    Rectangle { x: 8; y: 282; width: root.width - 16; height: 30; radius: 3; color: "#121a20"; border.color: "#28353d"
        Row { anchors.centerIn: parent; spacing: 5
            Repeater { model: ["DRIVE", "TIMING", "VEHICLE", "TYRES", "GRAPHS"]
                Rectangle { required property string modelData; width: 83; height: 23; radius: 2; color: index === root.page ? "#2a2210" : "#0e151a"; border.color: index === root.page ? root.accent : "#28353d"
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
