import QtQuick

Rectangle {
    id: plot
    required property string title
    required property string legend
    required property var samples
    required property string firstKey
    required property string secondKey
    required property color firstColor
    required property color secondColor
    required property real minimum
    required property real maximum
    color: "#10161b"; border.color: "#27323a"; radius: 3
    Text { x: 8; y: 5; text: plot.title; color: "#9dacb5"; font.pixelSize: 10; font.bold: true }
    Text { anchors.right: parent.right; anchors.rightMargin: 8; y: 5; text: plot.legend; color: "#9dacb5"; font.pixelSize: 9 }
    Canvas {
        id: canvas
        x: 8; y: 22; width: parent.width-16; height: parent.height-29
        onPaint: {
            const ctx = getContext("2d")
            const now = Date.now()
            ctx.clearRect(0,0,width,height)
            ctx.fillStyle = "#0c1115"; ctx.fillRect(0,0,width,height)
            ctx.strokeStyle = "#29343b"; ctx.lineWidth = 1
            ctx.beginPath()
            for (let row=0; row<=4; ++row) {
                const y = 0.5 + (height-1)*row/4
                ctx.moveTo(0,y); ctx.lineTo(width,y)
            }
            for (let col=0; col<=6; ++col) {
                const x = 0.5 + (width-1)*col/6
                ctx.moveTo(x,0); ctx.lineTo(x,height)
            }
            ctx.stroke()
            for (const line of [{key:plot.firstKey,color:plot.firstColor},{key:plot.secondKey,color:plot.secondColor}]) {
                ctx.strokeStyle = line.color; ctx.lineWidth=2; ctx.beginPath()
                let drawing=false
                for (const sample of plot.samples) {
                    const v=sample[line.key]
                    if (v == null || !Number.isFinite(Number(v))) { drawing=false; continue }
                    const x=width*(1-(now-Number(sample.time))/30000)
                    const y=height*(1-Math.max(0,Math.min(1,(Number(v)-plot.minimum)/(plot.maximum-plot.minimum))))
                    if (drawing) ctx.lineTo(x,y)
                    else { ctx.moveTo(x,y); drawing=true }
                }
                ctx.stroke()
            }
        }
        onAvailableChanged: if (available) requestPaint()
    }
    onSamplesChanged: if (visible) canvas.requestPaint()
    onVisibleChanged: if (visible) canvas.requestPaint()
    Timer { interval: 200; running: plot.visible; repeat: true; onTriggered: canvas.requestPaint() }
}
