//
//  OBSCameraProviderSource.swift
//  camera-extension
//
//  Created by Sebastian Beckmann on 2022-09-30.
//  Changed by Patrick Heyer on 2022-10-16.
//

import CoreMediaIO
import Foundation

// The name every camera consumer renders in its device picker -- Zoom, Meet, FaceTime, Safari --
// and the name macOS prints in System Settings > Camera Extensions.
//
// It is read out of this bundle's own Info.plist, exactly as main.swift already reads the three
// device UUIDs, instead of being typed here. Upstream hardcoded "OBS Virtual Camera" in this file
// AND in INFOPLIST_KEY_CFBundleDisplayName, and the fork's re-mint changed neither: the machine
// -readable identity (bundle id, Mach service, device/source/sink UUIDs) was forked while the only
// strings a human ever sees stayed byte-identical to stock OBS's, whose extension is live on this
// machine. Deriving the name means there is one string to change (camera-extension/CMakeLists.txt)
// and one string for the configure-time identity guard to assert on -- a Swift literal is not
// visible to CMake, so a second copy here could never be checked.
private let OBSCameraDeviceName: String = {
    guard let name = Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String, !name.isEmpty else {
        // Publishing a nameless device would be worse than not publishing one: it shows up in every
        // picker on the machine as a blank row the operator cannot identify or avoid.
        fatalError("Camera Extension bundle has no CFBundleDisplayName; refusing to publish an unnamed device.")
    }
    return name
}()

class OBSCameraProviderSource: NSObject, CMIOExtensionProviderSource {
    private(set) var provider: CMIOExtensionProvider!

    private var deviceSource: OBSCameraDeviceSource!

    init(clientQueue: DispatchQueue?, deviceUUID: UUID, sourceUUID: UUID, sinkUUID: UUID) {
        super.init()

        provider = CMIOExtensionProvider(source: self, clientQueue: clientQueue)
        deviceSource = OBSCameraDeviceSource(
            localizedName: OBSCameraDeviceName,
            deviceUUID: deviceUUID,
            sourceUUID: sourceUUID,
            sinkUUID: sinkUUID)
        do {
            try provider.addDevice(deviceSource.device)
        } catch let error {
            fatalError("Failed to add device \(error.localizedDescription)")
        }
    }

    func connect(to client: CMIOExtensionClient) throws {
    }

    func disconnect(from client: CMIOExtensionClient) {
    }

    var availableProperties: Set<CMIOExtensionProperty> {
        return [.providerName, .providerManufacturer]
    }

    func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionProviderProperties
    {
        let providerProperties = CMIOExtensionProviderProperties(dictionary: [:])

        if properties.contains(.providerName) {
            providerProperties.name = "\(OBSCameraDeviceName) Provider"
        }

        if properties.contains(.providerManufacturer) {
            // Not "OBS Project". This fork is signed by team V46GH2Q974 and is not shipped, supported
            // or endorsed by the OBS Project; reporting their name as the manufacturer of a system
            // extension misattributes it to them in every tool that surfaces CMIO provider metadata.
            providerProperties.manufacturer = "Zoetic Solutions"
        }

        return providerProperties
    }

    func setProviderProperties(_ providerProperties: CMIOExtensionProviderProperties) throws {
    }
}
