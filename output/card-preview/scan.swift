import Foundation
import Vision
import ImageIO
let url = URL(fileURLWithPath: CommandLine.arguments[1])
let source = CGImageSourceCreateWithURL(url as CFURL, nil)!
let image = CGImageSourceCreateImageAtIndex(source, 0, nil)!
let request = VNDetectBarcodesRequest()
request.symbologies = [.qr]
try VNImageRequestHandler(cgImage: image).perform([request])
guard let value = request.results?.first?.payloadStringValue, value.hasPrefix("BEGIN:VCARD\r\nVERSION:3.0"), value.contains("TEL:+65 12345678") else { fatalError("QR contact did not round-trip") }
print("Rendered contact QR round-trip: PASS")
