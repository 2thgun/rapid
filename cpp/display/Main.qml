import QtQuick
import QtQuick.Window

Window {
    id: root
    width: 480; height: 320
    visible: true
    flags: Qt.FramelessWindowHint
    color: "#070a0d"
    property int page: 0
    property int revision: dashboard.revision
    property bool wifiMenu: false
    readonly property color accent: "#f6b91a"
    readonly property color muted: "#9dacb5"
    function raw(key) { revision; return dashboard.value(key) }
    function text(key) { const v = raw(key); return v === undefined || v === null || v === "" ? "—" : String(v) }
    function numeric(key, digits, suffix) {
        const v = raw(key)
        return v === undefined || v === null || !Number.isFinite(Number(v)) ? "—" : Number(v).toFixed(digits) + (suffix || "")
    }
    function number(key) { const v = raw(key); return v == null ? 0 : Number(v) || 0 }
    function time(key) { revision; return dashboard.timeValue(key) }
    function percent(key) { revision; return dashboard.percentValue(key) }
    function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)) }
    function delta(key) {
        const v = raw(key)
        if (v == null) return "—"
        const ms = Math.abs(Number(v))
        return (v > 0 ? "+" : v < 0 ? "-" : "") + Math.floor(ms/60000) + ":" + String(Math.floor(ms/1000)%60).padStart(2,"0") + "." + String(ms%1000).padStart(3,"0")
    }
    function sector(n) { return time("sector_" + n + "_ms") + (raw("sector_" + n + "_delta_ms") == null ? "" : " (" + delta("sector_" + n + "_delta_ms") + ")") }
    function pair(a, b, suffix) { return numeric(a,1) + " / " + numeric(b,1) + (suffix || "") }
    function gear() { const v = raw("gear"); return v == null ? "—" : v === -1 ? "R" : v === 0 ? "N" : String(v) }

    component Label: Text {
        color: root.muted; font.pixelSize: 10; font.bold: true
        elide: Text.ElideRight
    }
    component Card: Rectangle {
        radius: 3; color: "#10161b"; border.color: "#27323a"
    }
    component Metric: Column {
        required property string caption
        required property string reading
        spacing: 3
        Label { width: parent.width; text: parent.caption }
        Text { width: parent.width; text: parent.reading; color: "#f4f7f9"; font.pixelSize: 13; font.bold: true; elide: Text.ElideRight }
    }
    component MetricCard: Card {
        required property string title
        required property var entries
        property int columns: 2
        Label { x: 8; y: 8; width: parent.width-16; text: parent.title }
        Grid {
            x: 8; y: 31; width: parent.width-16
            columns: parent.columns; columnSpacing: 6; rowSpacing: 9
            Repeater {
                model: parent.parent.entries
                Metric {
                    required property var modelData
                    width: (parent.width - (parent.columns-1)*6) / parent.columns
                    caption: modelData[0]; reading: String(modelData[1])
                }
            }
        }
    }

    Card {
        x: 6; y: 4; width: 468; height: 36
        Label { x: 8; width: 274; anchors.verticalCenter: parent.verticalCenter; text: dashboard.status; font.pixelSize: 11 }
        Label { x: 286; width: 72; anchors.verticalCenter: parent.verticalCenter
            text: !root.raw("power_status_available") ? "PWR ?" : root.raw("power_limited") ? "PWR LIMIT" : root.raw("power_limited_since_boot") ? "PWR WARN" : "PWR OK"
            color: root.raw("power_limited") ? "#ff6472" : root.muted
        }
        Rectangle {
            x: 362; y: 2; width: 104; height: 32; radius: 4
            color: wifiTouch.pressed ? "#403519" : "#221c0d"; border.color: root.accent; border.width: 2
            Label { anchors.centerIn: parent; text: "WIFI " + (dashboard.networkMode || "?").toUpperCase() + " ▾"; color: root.accent }
            MouseArea { id: wifiTouch; anchors.fill: parent; onClicked: root.wifiMenu = !root.wifiMenu }
        }
    }
    Item {
        id: body
        x: 6; y: 46; width: 468; height: 214
        Row {
            visible: root.page === 0; spacing: 6
            Card {
                width: 140; height: body.height
                Label { x: 8; y: 8; text: "GEAR" }
                Text { x: 8; y: 23; text: root.gear(); color: root.accent; font.pixelSize: 44; font.bold: true }
                Metric { x: 67; y: 30; width: 66; caption: "KM/H"; reading: root.numeric("speed_kmh",0) }
                Label { x: 8; y: 80; text: "RPM  " + root.text("rpm") }
                Rectangle { x: 8; y: 97; width: 124; height: 6; radius: 3; color: "#252d33"
                    Rectangle { width: parent.width * root.clamp(root.number("rpm")/10000,0,1); height: 6; radius: 3; color: root.accent }
                }
                Column { x: 8; y: 116; spacing: 12
                    Label { text: "LAP     " + root.time("current_lap_ms"); color: "#f4f7f9" }
                    Label { text: "LAST   " + root.time("completed_lap_ms"); color: "#f4f7f9" }
                    Label { text: "DELTA " + root.delta("delta_ms"); color: "#f4f7f9" }
                    Label { text: root.raw("recording") ? "REC" : "IDLE"; color: root.raw("recording") ? "#20cf75" : root.muted }
                }
            }
            Card {
                width: 182; height: body.height
                Repeater { model: [["THROTTLE","throttle","#20cf75"],["BRAKE","brake","#ef4458"]]
                    Item { required property var modelData; required property int index; x: 8; y: 10+index*45; width: 166; height: 40
                        Label { text: modelData[0] }
                        Label { anchors.right: parent.right; text: root.percent(modelData[1]); color: "#f4f7f9" }
                        Rectangle { y: 21; width: 166; height: 9; radius: 3; color: "#252d33"
                            Rectangle { width: parent.width * root.clamp(root.number(modelData[1]),0,1); height: 9; radius: 3; color: modelData[2] }
                        }
                    }
                }
                Metric { x: 8; y: 129; width: 76; caption: "STEERING"; reading: root.raw("steering_angle") == null ? "—" : (root.number("steering_angle")*180/Math.PI).toFixed(0)+"°" }
                Image { x: 88; y: 113; width: 86; height: 86; source: "qrc:/assets/steering-wheel-cartoon.png"; fillMode: Image.PreserveAspectFit; rotation: root.number("steering_angle")*180/Math.PI }
            }
            Card {
                width: 134; height: body.height
                Label { x: 8; y: 8; text: "G FORCE" }
                Rectangle { x: 20; y: 31; width: 94; height: 94; radius: 47; color: "#10161b"; border.color: "#3d4b54"; border.width: 2
                    Rectangle { x: 46; y: 3; width: 1; height: 88; color: "#29343b" }
                    Rectangle { x: 3; y: 46; width: 88; height: 1; color: "#29343b" }
                    Rectangle { x: 41+root.clamp(root.number("g_x")/2,-1,1)*38; y: 41+root.clamp(root.number("g_z")/2,-1,1)*38; width: 12; height: 12; radius: 6; color: "#34bdf2" }
                }
                Label { x: 8; y: 137; width: 118; text: root.pair("g_x","g_z"," g"); horizontalAlignment: Text.AlignHCenter; color: "#f4f7f9" }
                Label { x: 8; y: 160; text: "LAT / LONG" }
                Label { x: 8; y: 190; text: "LAP  " + root.text("lap_number"); color: root.accent }
            }
        }
        Row {
            visible: root.page === 1; spacing: 6
            MetricCard { width: 190; height: body.height; title: "LIVE TIMING"
                entries: [["CURRENT",root.time("current_lap_ms")],["LAST",root.time("completed_lap_ms")],["BEST",root.time("best_lap_ms")],["DELTA",root.delta("delta_ms")],["S1",root.sector(1)],["S2",root.sector(2)],["S3",root.sector(3)],["LAP",root.text("lap_number")]]
            }
            MetricCard { width: 146; height: body.height; columns: 1; title: "SESSION"
                entries: [["TRACK",root.text("track_name")],["CAR",root.text("car_model")],["DRIVER",root.text("driver_name")],["POSITION",root.raw("lap_position") == null ? "—" : Math.round(root.number("lap_position")*(root.number("lap_position")>1 ? 1 : 100))+"%"]]
            }
            MetricCard { width: 120; height: body.height; columns: 1; title: "RECORDING"
                entries: [["STATE",root.raw("recording") ? "REC" : "IDLE"],["SAMPLES",root.text("recorded_samples")],["SIM",root.text("simulator")]]
            }
        }
        Row {
            visible: root.page === 2; spacing: 6
            MetricCard { width: 152; height: body.height; title: "CAR STATE"
                entries: [["FUEL",root.numeric("fuel",1," L")],["TC",root.text("tc")],["ABS",root.percent("abs_activity")],["LIMITER",root.raw("pit_limiter") == null ? "—" : root.raw("pit_limiter") ? "ON" : "OFF"]]
            }
            MetricCard { width: 152; height: body.height; columns: 1; title: "WHEEL SPEED"
                entries: [["FL / FR",root.pair("wheel_speed_fl","wheel_speed_fr"," km/h")],["RL / RR",root.pair("wheel_speed_rl","wheel_speed_rr"," km/h")]]
            }
            MetricCard { width: 152; height: body.height; columns: 1; title: "DAMAGE"
                entries: [["FRONT",root.percent("damage_front")],["REAR",root.percent("damage_rear")]]
            }
        }
        Row {
            visible: root.page === 3; spacing: 6
            MetricCard { width: 214; height: body.height; title: "TYRE CORE / PRESSURE"
                entries: ["fl","fr","rl","rr"].map(c => [c.toUpperCase(),root.numeric("core_temp_"+c,0," C")+" / "+root.numeric("pressure_"+c,1)])
            }
            MetricCard { width: 152; height: body.height; columns: 1; title: "SUSPENSION"
                entries: [["FRONT",root.pair("suspension_fl","suspension_fr")],["REAR",root.pair("suspension_rl","suspension_rr")]]
            }
            MetricCard { width: 90; height: body.height; columns: 1; title: "STATUS"
                entries: [["SOURCE",root.text("simulator")],["DATA",root.raw("telemetry_fresh") ? "LIVE" : "WAITING"]]
            }
        }
        Column {
            visible: root.page === 4; spacing: 6
            HistoryPlot { width: body.width; height: 104; title: "PEDALS"; legend: "THROTTLE / BRAKE   •   30 SEC"; samples: dashboard.graphSamples; firstKey: "throttle"; secondKey: "brake"; firstColor: "#20cf75"; secondColor: "#ef4458"; minimum: 0; maximum: 100 }
            HistoryPlot { width: body.width; height: 104; title: "G FORCE"; legend: "LATERAL / LONG.   •   ±2.5 G"; samples: dashboard.graphSamples; firstKey: "lateral"; secondKey: "longitudinal"; firstColor: "#34bdf2"; secondColor: "#f6b91a"; minimum: -2.5; maximum: 2.5 }
        }
    }
    Row {
        x: 6; y: 266; spacing: 5
        Repeater { model: ["DRIVE","TIMING","VEHICLE","TYRES","GRAPHS"]
            Rectangle { required property string modelData; required property int index
                width: 89.6; height: 48; radius: 4
                color: tabTouch.pressed ? "#403519" : index === root.page ? "#221c0d" : "#10161b"
                border.color: index === root.page ? root.accent : "#394754"
                Label { anchors.centerIn: parent; text: modelData; color: index === root.page ? root.accent : root.muted; font.pixelSize: 11 }
                MouseArea { id: tabTouch; anchors.fill: parent; onClicked: root.page = index }
            }
        }
    }
    Card { visible: dashboard.logNotice.length > 0; x: 165; y: 241; width: 135; height: 19; z: 2
        Label { anchors.centerIn: parent; text: dashboard.logNotice; color: "#20cf75" }
    }
    Rectangle {
        visible: root.wifiMenu; anchors.fill: parent; color: "#b0000000"; z: 10
        MouseArea { anchors.fill: parent; onClicked: root.wifiMenu = false }
        Card { x: 30; y: 90; width: 420; height: 130
            Label { x: 14; y: 14; text: "WI-FI MODE"; font.pixelSize: 14 }
            Row { x: 12; y: 48; spacing: 8
                Repeater { model: [["home","HOME"],["ap","ACCESS POINT"],["off","WI-FI OFF"]]
                    Rectangle { required property var modelData; width: 126; height: 62; radius: 4
                        color: "#19242b"; border.width: 2; border.color: dashboard.networkMode === modelData[0] ? root.accent : "#52616b"
                        Label { anchors.centerIn: parent; text: modelData[1]; color: "#f4f7f9"; font.pixelSize: 12 }
                        MouseArea { anchors.fill: parent; onClicked: { dashboard.setNetworkMode(modelData[0]); root.wifiMenu = false } }
                    }
                }
            }
        }
    }
}
