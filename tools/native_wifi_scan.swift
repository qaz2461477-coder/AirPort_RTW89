import Foundation
import CoreWLAN

// One active scan through CoreWLAN, the path an app takes. Prints how many
// networks came back; names need Location access, and are not needed here.
var rows: [String: Any] = [:]
if let interface = CWWiFiClient.shared().interface() {
    rows["interface"] = interface.interfaceName ?? ""
    do {
        let networks = try interface.scanForNetworks(withName: nil)
        rows["networks"] = networks.count
        rows["channels"] = Array(Set(networks.compactMap { $0.wlanChannel?.channelNumber })).sorted()
    } catch {
        rows["error"] = String(describing: error)
    }
}
let json = try JSONSerialization.data(withJSONObject: rows, options: [.sortedKeys])
print(String(decoding: json, as: UTF8.self))
