import QtQuick 2.11
import QtQuick.Controls 1.4

Item {
    id: root
    objectName: "rootrotoscene"

    SystemPalette { id: activePalette }
    // default size, but scalable by user
    height: 300; width: 400
    property string comment
    property string framenum
    property point profile
    property point center
    property real baseUnit: fontMetrics.font.pixelSize * 0.8
    property double scalex : 1
    property double scaley : 1
    property double stretch : 1
    property double sourcedar : 1
    property double offsetx : 0
    property double offsety : 0
    property double frameSize: 10
    property int duration: 300
    property double timeScale: 1
    property int mouseRulerPos: 0
    onOffsetxChanged: canvas.requestPaint()
    onOffsetyChanged: canvas.requestPaint()
    onScalexChanged: canvas.requestPaint()
    onScaleyChanged: canvas.requestPaint()
    onSourcedarChanged: refreshdar()
    property bool iskeyframe : true
    property bool isDefined: false
    property int requestedKeyFrame : -1
    property int requestedSubKeyFrame : -1
    property bool requestedCenter : false
    // The coordinate points where the bezier curve passes
    property var centerPoints : []
    property var centerCross : []
    // The control points for the bezier curve points (2 controls points for each coordinate)
    property var centerPointsTypes : []
    property bool showToolbar: false
    onCenterPointsTypesChanged: checkDefined()
    signal effectPolygonChanged()
    signal addKeyframe()
    signal seekToKeyframe()

    onDurationChanged: {
        clipMonitorRuler.updateRuler()
    }
    onWidthChanged: {
        clipMonitorRuler.updateRuler()
    }

    onIskeyframeChanged: {
        console.log('KEYFRAME CHANGED: ', iskeyframe)
        canvas.requestPaint()
    }

    FontMetrics {
        id: fontMetrics
        font.family: "Arial"
    }

    function refreshdar() {
        canvas.darOffset = root.sourcedar < root.profile.x * root.stretch / root.profile.y ? (root.profile.x * root.stretch - root.profile.y * root.sourcedar) / (2 * root.profile.x * root.stretch) :(root.profile.y - root.profile.x * root.stretch / root.sourcedar) / (2 * root.profile.y);
        canvas.requestPaint()
    }

    function checkDefined() {
        root.isDefined = root.centerPointsTypes.length > 0
        canvas.requestPaint()
    }

    Item {
        id: monitorOverlay
        height: root.height - controller.rulerHeight
        width: root.width

    Canvas {
      id: canvas
      property double handleSize
      property double darOffset : 0
      anchors.fill: parent
      contextType: "2d";
      handleSize: root.baseUnit / 2
      renderTarget: Canvas.FramebufferObject
      renderStrategy: Canvas.Cooperative

      onPaint:
      {
        var ctx = getContext('2d')
        //if (context) {
            ctx.clearRect(0,0, width, height);
            ctx.beginPath()
            ctx.strokeStyle = Qt.rgba(1, 0, 0, 0.5)
            ctx.fillStyle = Qt.rgba(1, 0, 0, 0.5)
            ctx.lineWidth = 2
            if (root.centerPoints.length == 0) {
                // no points defined yet
                return
            }
            var p1 = convertPoint(root.centerPoints[0])
            var startP = p1;
            ctx.moveTo(p1.x, p1.y)
            if (!isDefined) {
                ctx.fillRect(p1.x - handleSize, p1.y - handleSize, 2 * handleSize, 2 * handleSize);
                for (var i = 1; i < root.centerPoints.length; i++) {
                    p1 = convertPoint(root.centerPoints[i])
                    ctx.lineTo(p1.x, p1.y);
                    ctx.fillRect(p1.x - handleSize, p1.y - handleSize, 2 * handleSize, 2 * handleSize);
                }
            } else {
                var c1; var c2
                var topRight = []
                var bottomLeft = []
                for (var i = 0; i < root.centerPoints.length; i++) {
                    p1 = convertPoint(root.centerPoints[i])
                    // Control points
                    var subkf = false
                    if (i == 0) {
                        c1 = convertPoint(root.centerPointsTypes[root.centerPointsTypes.length - 1])
                        if (root.requestedSubKeyFrame == root.centerPointsTypes.length - 1) {
                            subkf = true
                        }
                        topRight.x = p1.x
                        topRight.y = p1.y
                        bottomLeft.x = p1.x
                        bottomLeft.y = p1.y
                    } else {
                        c1 = convertPoint(root.centerPointsTypes[2*i - 1])
                        if (root.requestedSubKeyFrame == 2*i - 1) {
                            subkf = true
                        }
                        // Find bounding box
                        topRight.x = Math.max(p1.x, topRight.x)
                        topRight.y = Math.min(p1.y, topRight.y)
                        bottomLeft.x = Math.min(p1.x, bottomLeft.x)
                        bottomLeft.y = Math.max(p1.y, bottomLeft.y)
                    }
                    c2 = convertPoint(root.centerPointsTypes[2*i])
                    ctx.bezierCurveTo(c1.x, c1.y, c2.x, c2.y, p1.x, p1.y);
                    if (iskeyframe) {
                        if (subkf) {
                            ctx.fillStyle = Qt.rgba(1, 1, 0, 0.8)
                            ctx.fillRect(c1.x - handleSize/2, c1.y - handleSize/2, handleSize, handleSize);
                            ctx.fillStyle = Qt.rgba(1, 0, 0, 0.5)
                        } else {
                            ctx.fillRect(c1.x - handleSize/2, c1.y - handleSize/2, handleSize, handleSize);
                        }
                        if (root.requestedSubKeyFrame == 2 * i) {
                            ctx.fillStyle = Qt.rgba(1, 1, 0, 0.8)
                            ctx.fillRect(c2.x - handleSize/2, c2.y - handleSize/2, handleSize, handleSize);
                            ctx.fillStyle = Qt.rgba(1, 0, 0, 0.5)
                        } else {
                            ctx.fillRect(c2.x - handleSize/2, c2.y - handleSize/2, handleSize, handleSize);
                        }
                        c1 = convertPoint(root.centerPointsTypes[2*i + 1])
                        ctx.lineTo(c1.x, c1.y);
                        ctx.moveTo(p1.x, p1.y)
                        ctx.lineTo(c2.x, c2.y);
                        ctx.moveTo(p1.x, p1.y)
                        if (i == root.requestedKeyFrame) {
                            ctx.fillStyle = Qt.rgba(1, 1, 0, 0.8)
                            ctx.fillRect(p1.x - handleSize, p1.y - handleSize, 2 * handleSize, 2 * handleSize);
                            ctx.fillStyle = Qt.rgba(1, 0, 0, 0.5)
                        } else {
                            ctx.fillRect(p1.x - handleSize, p1.y - handleSize, 2 * handleSize, 2 * handleSize);
                        }
                    }
                }
                if (root.centerPoints.length > 2) {
                    c1 = convertPoint(root.centerPointsTypes[root.centerPointsTypes.length - 1])
                    c2 = convertPoint(root.centerPointsTypes[0])
                    ctx.bezierCurveTo(c1.x, c1.y, c2.x, c2.y, startP.x, startP.y);
                }
                centerCross.x = bottomLeft.x + (topRight.x - bottomLeft.x)/2
                centerCross.y = topRight.y + (bottomLeft.y - topRight.y)/2
                ctx.moveTo(centerCross.x - root.baseUnit, centerCross.y)
                ctx.lineTo(centerCross.x + root.baseUnit, centerCross.y)
                ctx.moveTo(centerCross.x, centerCross.y - root.baseUnit)
                ctx.lineTo(centerCross.x, centerCross.y + root.baseUnit)
            }

            ctx.stroke()
    }

    function convertPoint(p)
    {
        var x = frame.x + p.x * root.scalex
        var y = frame.y + p.y * root.scaley
        return Qt.point(x,y);
    }
  }

    Rectangle {
        id: frame
        objectName: "referenceframe"
        property color hoverColor: "#ff0000"
        width: root.profile.x * root.scalex
        height: root.profile.y * root.scaley
        x: root.center.x - width / 2 - root.offsetx;
        y: root.center.y - height / 2 - root.offsety;
        color: "transparent"
        border.color: "#ffffff00"
    }

    Rectangle {
        anchors.centerIn: parent
        width: label.contentWidth + 6
        height: label.contentHeight + 6
        visible: !root.isDefined && !global.containsMouse
        opacity: 0.8
        Text {
            id: label
            text: i18n("Click to add points,\nright click to close shape.")
            font.pointSize: root.baseUnit
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            anchors {
                fill: parent
            }
            color: 'black'
         }
        color: "yellow"
    }

    MouseArea {
        id: global
        objectName: "global"
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        anchors.fill: parent
        property bool pointContainsMouse
        property bool centerContainsMouse
        hoverEnabled: true
        cursorShape: !root.isDefined ? Qt.PointingHandCursor : (pointContainsMouse || centerContainsMouse) ? Qt.PointingHandCursor : Qt.ArrowCursor

        onClicked: {
            if (!root.isDefined) {
                if (mouse.button == Qt.RightButton && root.centerPoints.length > 2) {
                    // close shape, define control points
                    var p0; var p1; var p2
                    for (var i = 0; i < root.centerPoints.length; i++) {
                        p1 = root.centerPoints[i]
                        if (i == 0) {
                            p0 = root.centerPoints[root.centerPoints.length - 1]
                        } else {
                            p0 = root.centerPoints[i - 1]
                        }
                        if (i == root.centerPoints.length - 1) {
                            p2 = root.centerPoints[0]
                        } else {
                            p2 = root.centerPoints[i + 1]
                        }
                        var ctrl1 = Qt.point((p0.x - p1.x) / 5, (p0.y - p1.y) / 5);
                        var ctrl2 = Qt.point((p2.x - p1.x) / 5, (p2.y - p1.y) / 5);
                        root.centerPointsTypes.push(Qt.point(p1.x + ctrl1.x, p1.y + ctrl1.y))
                        root.centerPointsTypes.push(Qt.point(p1.x + ctrl2.x, p1.y + ctrl2.y))
                    }
                    root.isDefined = true;
                    root.effectPolygonChanged()
                    canvas.requestPaint()
                } else {
                    var newPoint = Qt.point((mouseX - frame.x) / root.scalex, (mouseY - frame.y) / root.scaley);
                    root.centerPoints.push(newPoint)
                    canvas.requestPaint()
                }
            }
        }

        onDoubleClicked: {
            root.addKeyframe()
        }

        onPositionChanged: {
            if (root.iskeyframe == false) return;
            if (isDefined && pressed) {
                if (centerContainsMouse) {
                    var xDiff = (mouseX - centerCross.x) / root.scalex
                    var yDiff = (mouseY - centerCross.y) / root.scaley
                    for (var j = 0; j < root.centerPoints.length; j++) {
                        root.centerPoints[j].x += xDiff
                        root.centerPoints[j].y += yDiff
                        root.centerPointsTypes[j * 2].x += xDiff
                        root.centerPointsTypes[j * 2].y += yDiff
                        root.centerPointsTypes[j * 2 + 1].x += xDiff
                        root.centerPointsTypes[j * 2 + 1].y += yDiff
                    }
                    canvas.requestPaint()
                    root.effectPolygonChanged()
                    return
                }
                if (root.requestedKeyFrame >= 0) {
                    var xDiff = (mouseX - frame.x) / root.scalex - root.centerPoints[root.requestedKeyFrame].x
                    var yDiff = (mouseY - frame.y) / root.scaley - root.centerPoints[root.requestedKeyFrame].y
                    root.centerPoints[root.requestedKeyFrame].x += xDiff
                    root.centerPoints[root.requestedKeyFrame].y += yDiff
                    root.centerPointsTypes[root.requestedKeyFrame * 2].x += xDiff
                    root.centerPointsTypes[root.requestedKeyFrame * 2].y += yDiff
                    root.centerPointsTypes[root.requestedKeyFrame * 2 + 1].x += xDiff
                    root.centerPointsTypes[root.requestedKeyFrame * 2 + 1].y += yDiff
                    canvas.requestPaint()
                    root.effectPolygonChanged()
                } else if (root.requestedSubKeyFrame >= 0) {
                    root.centerPointsTypes[root.requestedSubKeyFrame].x = (mouseX - frame.x) / root.scalex
                    root.centerPointsTypes[root.requestedSubKeyFrame].y = (mouseY - frame.y) / root.scaley
                    canvas.requestPaint()
                    root.effectPolygonChanged()
                }
            } else if (root.centerPoints.length > 0) {
              for(var i = 0; i < root.centerPoints.length; i++)
              {
                var p1 = canvas.convertPoint(root.centerPoints[i])
                if (Math.abs(p1.x - mouseX) <= canvas.handleSize && Math.abs(p1.y - mouseY) <= canvas.handleSize) {
                    if (i == root.requestedKeyFrame) {
                        centerContainsMouse = false
                        pointContainsMouse = true;
                        return;
                    }
                    root.requestedKeyFrame = i
                    canvas.requestPaint()
                    centerContainsMouse = false
                    pointContainsMouse = true;
                    return;
                }
              }
              for(var i = 0; i < root.centerPointsTypes.length; i++)
              {
                var p1 = canvas.convertPoint(root.centerPointsTypes[i])
                if (Math.abs(p1.x - mouseX) <= canvas.handleSize/2 && Math.abs(p1.y - mouseY) <= canvas.handleSize/2) {
                    if (i == root.requestedSubKeyFrame) {
                        centerContainsMouse = false
                        pointContainsMouse = true;
                        return;
                    }
                    root.requestedSubKeyFrame = i
                    canvas.requestPaint()
                    centerContainsMouse = false
                    pointContainsMouse = true;
                    return;
                } 
              }
              if (Math.abs(centerCross.x - mouseX) <= canvas.handleSize/2 && Math.abs(centerCross.y - mouseY) <= canvas.handleSize/2) {
                    centerContainsMouse = true;
                    pointContainsMouse = false;
                    return;
              }

              if (root.requestedKeyFrame == -1 && root.requestedSubKeyFrame == -1) {
                  return;
              }
              root.requestedKeyFrame = -1
              root.requestedSubKeyFrame = -1
              pointContainsMouse = false;
              centerContainsMouse = false
              canvas.requestPaint()
            }
        }
    }
}
    EffectToolBar {
        id: effectToolBar
        barContainsMouse: effectToolBar.rightSide ? global.mouseX >= x - 10 : global.mouseX < x + width + 10
        onBarContainsMouseChanged: {
            effectToolBar.opacity = 1
            effectToolBar.visible = effectToolBar.barContainsMouse
        }
        anchors {
            right: parent.right
            top: parent.top
            topMargin: 4
            rightMargin: 4
            leftMargin: 4
        }
    }
    MonitorRuler {
        id: clipMonitorRuler
        anchors {
            left: root.left
            right: root.right
            bottom: root.bottom
        }
        height: controller.rulerHeight
    }

}
