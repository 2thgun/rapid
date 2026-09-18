// Regression checks for Main.qml's display-only unit conversions (#18, #24).
//
// This file used to be dashboard_graph_tests.js and also exercised the now
// -removed browser dashboard (cpp/assets/dashboard.html): its own copy of the
// wheel-speed/steering-degrees formulas, its graph-freshness/gap logic, and
// its live-push/fallback-poll coalescing. The Qt panel (Main.qml) is the only
// live dashboard now (#24); those dashboard.html-only checks are gone with
// the file, and telemetry.html -- the page that remains at /telemetry -- has
// no equivalent wheel, steering or graph-freshness logic of its own to port
// (it never had a copy of this file's checks either). Its own /telemetry and
// WebSocket (?history= and ?mode=state) coverage lives in
// cpp/tests/native_network_tests.cpp, next to the rest of the HTTP surface.
const fs = require('fs');
const path = require('path');
const qmlSource = fs.readFileSync(path.join(__dirname, '../Main.qml'), 'utf8');

if (!qmlSource.includes('root.pair("wheel_speed_fl","wheel_speed_fr"," rad/s")') ||
    qmlSource.includes('root.pair("wheel_speed_fl","wheel_speed_fr"," km/h")')) {
  throw new Error('Main.qml wheel angular-speed displays must use rad/s');
}
if (qmlSource.includes('root.number("steering_angle")*180/Math.PI') ||
    qmlSource.includes('steering_angle") === "iRacing"')) {
  throw new Error('Main.qml steering wheel must not assume radians or single out one simulator');
}
if (!qmlSource.includes('steering_lock_deg')) {
  throw new Error('Main.qml steering wheel must derive degrees from the steering lock channel');
}

console.log('Main.qml wheel-speed unit and steering-lock degrees checks passed');
