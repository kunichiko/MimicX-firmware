// SysEx を送って、返ってくる SysEx を待つ最小ツール。
//   usage: midiprobe "<名前の部分一致>" <待ち秒数> F0 7D 01 0A F7
import Foundation
import CoreMIDI

func endpoints(_ isSource: Bool) -> [(MIDIEndpointRef, String)] {
    var out: [(MIDIEndpointRef, String)] = []
    let n = isSource ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations()
    for i in 0..<n {
        let ep = isSource ? MIDIGetSource(i) : MIDIGetDestination(i)
        var cf: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cf)
        out.append((ep, (cf?.takeRetainedValue() as String?) ?? "?"))
    }
    return out
}

let args = Array(CommandLine.arguments.dropFirst())
guard args.count >= 3 else { print("usage: midiprobe <name> <seconds> <hex...>"); exit(1) }
let needle = args[0]
let waitSec = Double(args[1]) ?? 2.0
let bytes = args.dropFirst(2).map { UInt8($0, radix: 16) ?? 0 }

guard let (dst, dstName) = endpoints(false).first(where: { $0.1.contains(needle) }),
      let (src, _) = endpoints(true).first(where: { $0.1.contains(needle) }) else {
    print("endpoint not found: \(needle)"); exit(1)
}

var client = MIDIClientRef()
MIDIClientCreate("midiprobe" as CFString, nil, nil, &client)

var inPort = MIDIPortRef()
MIDIInputPortCreateWithBlock(client, "in" as CFString, &inPort) { pktList, _ in
    for pkt in pktList.unsafeSequence() {
        var hex: [String] = []
        withUnsafeBytes(of: pkt.pointee.data) { raw in
            for i in 0..<Int(pkt.pointee.length) {
                hex.append(String(format: "%02X", raw[i]))
            }
        }
        print("  RX: " + hex.joined(separator: " "))
    }
}
MIDIPortConnectSource(inPort, src, nil)

var outPort = MIDIPortRef()
MIDIOutputPortCreate(client, "out" as CFString, &outPort)
var pl = MIDIPacketList()
var p = MIDIPacketListInit(&pl)
p = bytes.withUnsafeBufferPointer { b in
    MIDIPacketListAdd(&pl, 1024, p, 0, b.count, b.baseAddress!)
}
print("TX to \"\(dstName)\": " + bytes.map { String(format: "%02X", $0) }.joined(separator: " "))
MIDISend(outPort, dst, &pl)

RunLoop.current.run(until: Date().addingTimeInterval(waitSec))
print("done (waited \(waitSec)s)")
