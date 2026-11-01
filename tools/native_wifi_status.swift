import Foundation
import CoreWLAN

// Read-only discovery. No scan, association, power changes or SSID request.
let interfaces = CWWiFiClient.shared().interfaces() ?? []
let rows: [[String: Any]] = interfaces.map {
    ["interface": $0.interfaceName ?? "", "powerOn": $0.powerOn()]
}
let json = try JSONSerialization.data(withJSONObject: rows, options: [.prettyPrinted, .sortedKeys])
print(String(decoding: json, as: UTF8.self))
