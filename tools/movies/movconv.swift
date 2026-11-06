// movconv: converts the TMNT movies (MPEG-1 inside the AFS archives TMNT_V0x.DAT) to H.264 for the Vita's hardware decoder
// (rt/movie.c). Video only: the sound stays the game's own (Sofdec/ADX). Every movie is decoded back and compared with pl_mpeg.
// Usage: movconv <game dir> <out dir> [V00_00 ...]
//
// Output file TMNT_Vff_ee.avc (little endian):
//    0  "TMNTAVC1"
//    8  u32 version (1)
//   12  u32 width, 16 u32 height, 20 u32 frames
//   24  u32 fps numerator, 28 u32 fps denominator
//   32  u32 max_num_ref_frames (from the SPS)
//   36  u32 largest access unit in bytes
//   40  u32 index offset, 44 u32 data offset, 48..63 zero
//   index: per frame u32 offset, u32 size, u32 flags (bit 0: keyframe)
//   data: one Annex-B access unit per frame, in display order (no B-frames); keyframes start with SPS and PPS.
import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

let startCode: [UInt8] = [0, 0, 0, 1]

struct Fail: Error, CustomStringConvertible {
    let description: String
    init(_ s: String) { description = s }
}

// ------------------------------------------------------------------ SPS ---

/// The fields of the SPS that matter for the Vita decoder.
struct SpsInfo {
    var profile = 0, level = 0, maxRefFrames = 0, pocType = -1
    var reorderFrames = -1, decFrameBuffering = -1   // -1: no bitstream_restriction in the VUI
    var rbsp: [UInt8] = []
    var vuiFlagBit = -1, restrictionFlagBit = -1      // bit positions in rbsp (restriction: only when the VUI is there)
}

struct BitReader {
    let b: [UInt8]
    var pos = 0
    init(_ bytes: [UInt8]) { b = bytes }
    mutating func bit() throws -> Int {
        guard pos < b.count * 8 else { throw Fail("SPS ends early") }
        let v = Int(b[pos >> 3] >> (7 - UInt8(pos & 7))) & 1
        pos += 1
        return v
    }
    mutating func bits(_ n: Int) throws -> Int {
        var v = 0
        for _ in 0..<n { v = (v << 1) | (try bit()) }
        return v
    }
    mutating func ue() throws -> Int {
        var zeros = 0
        while try bit() == 0 { zeros += 1; if zeros > 31 { throw Fail("bad Exp-Golomb code") } }
        return (1 << zeros) - 1 + (try bits(zeros))
    }
    mutating func se() throws -> Int {
        let k = try ue()
        return k & 1 == 1 ? (k + 1) / 2 : -(k / 2)
    }
}

func parseSps(_ nal: Data) throws -> SpsInfo {
    // Drop the NAL header and the emulation prevention bytes (00 00 03).
    var rbsp: [UInt8] = []
    var zeros = 0
    for byte in nal.dropFirst() {
        if zeros >= 2 && byte == 3 { zeros = 0; continue }
        rbsp.append(byte)
        zeros = byte == 0 ? zeros + 1 : 0
    }
    var r = BitReader(rbsp)
    var info = SpsInfo()
    info.rbsp = rbsp
    info.profile = try r.bits(8)
    _ = try r.bits(8)
    info.level = try r.bits(8)
    _ = try r.ue()
    if [100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135].contains(info.profile) {
        throw Fail("profile \(info.profile) is not Main")
    }
    _ = try r.ue()   // log2_max_frame_num_minus4
    info.pocType = try r.ue()
    if info.pocType == 0 {
        _ = try r.ue()
    } else if info.pocType == 1 {
        _ = try r.bit()
        _ = try r.se()
        _ = try r.se()
        let n = try r.ue()
        for _ in 0..<n { _ = try r.se() }
    }
    info.maxRefFrames = try r.ue()
    _ = try r.bit()
    _ = try r.ue()
    _ = try r.ue()
    if try r.bit() == 0 { _ = try r.bit() }   // frame_mbs_only, mb_adaptive_frame_field
    _ = try r.bit()
    if try r.bit() == 1 { for _ in 0..<4 { _ = try r.ue() } }
    info.vuiFlagBit = r.pos
    guard try r.bit() == 1 else { return info }   // no VUI
    if try r.bit() == 1 { if try r.bits(8) == 255 { _ = try r.bits(32) } }
    if try r.bit() == 1 { _ = try r.bit() }
    if try r.bit() == 1 { _ = try r.bits(4); if try r.bit() == 1 { _ = try r.bits(24) } }
    if try r.bit() == 1 { _ = try r.ue(); _ = try r.ue() }
    if try r.bit() == 1 { _ = try r.bits(32); _ = try r.bits(32); _ = try r.bit() }
    func hrd() throws {
        let count = try r.ue() + 1
        _ = try r.bits(8)
        for _ in 0..<count { _ = try r.ue(); _ = try r.ue(); _ = try r.bit() }
        _ = try r.bits(20)
    }
    let nalHrd = try r.bit()
    if nalHrd == 1 { try hrd() }
    let vclHrd = try r.bit()
    if vclHrd == 1 { try hrd() }
    if nalHrd == 1 || vclHrd == 1 { _ = try r.bit() }
    _ = try r.bit()
    info.restrictionFlagBit = r.pos
    if try r.bit() == 1 {
        _ = try r.bit()
        for _ in 0..<4 { _ = try r.ue() }
        info.reorderFrames = try r.ue()
        info.decFrameBuffering = try r.ue()
    }
    return info
}

struct BitWriter {
    var bytes: [UInt8] = []
    var n = 0
    mutating func bit(_ v: Int) {
        if n % 8 == 0 { bytes.append(0) }
        if v != 0 { bytes[bytes.count - 1] |= UInt8(0x80 >> (n % 8)) }
        n += 1
    }
    mutating func ue(_ v: Int) {
        let x = v + 1
        let len = Int.bitWidth - x.leadingZeroBitCount
        for _ in 0..<(len - 1) { bit(0) }
        for i in stride(from: len - 1, through: 0, by: -1) { bit((x >> i) & 1) }
    }
}

/// The SPS with a VUI bitstream_restriction saying there is no reordering (max_num_reorder_frames 0), so a decoder hands out
/// each picture at once instead of filling its picture buffer first. Everything before the flag is copied bit for bit.
func spsWithoutReordering(_ nal: Data) throws -> Data {
    let info = try parseSps(nal)
    if info.reorderFrames == 0 { return nal }
    guard info.reorderFrames < 0 else { throw Fail("the stream reorders frames (\(info.reorderFrames))") }
    var src = BitReader(info.rbsp)
    var w = BitWriter()
    let cut = info.restrictionFlagBit >= 0 ? info.restrictionFlagBit : info.vuiFlagBit
    for _ in 0..<cut { w.bit(try src.bit()) }
    if info.restrictionFlagBit < 0 {
        // No VUI: one with nothing but the restriction.
        w.bit(1)
        for _ in 0..<8 { w.bit(0) }   // aspect ratio, overscan, video signal, chroma location, timing, NAL HRD, VCL HRD, pic_struct
    }
    w.bit(1)          // bitstream_restriction_flag
    w.bit(1)          // motion_vectors_over_pic_boundaries_flag
    w.ue(2)           // max_bytes_per_pic_denom (the value when absent)
    w.ue(1)           // max_bits_per_mb_denom
    w.ue(16)          // log2_max_mv_length_horizontal
    w.ue(16)          // log2_max_mv_length_vertical
    w.ue(0)           // max_num_reorder_frames
    w.ue(max(1, info.maxRefFrames))   // max_dec_frame_buffering
    w.bit(1)          // rbsp_stop_one_bit
    while w.n % 8 != 0 { w.bit(0) }
    // Back to a NAL: header byte, then emulation prevention.
    var out: [UInt8] = [nal.first!]
    var zeros = 0
    for byte in w.bytes {
        if zeros >= 2 && byte <= 3 { out.append(3); zeros = 0 }
        out.append(byte)
        zeros = byte == 0 ? zeros + 1 : 0
    }
    return Data(out)
}

/// NAL units of an Annex-B access unit (without start codes).
func nalUnits(_ au: Data) -> [Data] {
    let b = [UInt8](au)
    var starts: [(Int, Int)] = []   // (start code offset, payload offset)
    var i = 0
    while i + 3 <= b.count {
        if b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 {
            starts.append((i > 0 && b[i - 1] == 0 ? i - 1 : i, i + 3))
            i += 3
        } else {
            i += 1
        }
    }
    var out: [Data] = []
    for (k, s) in starts.enumerated() {
        let end = k + 1 < starts.count ? starts[k + 1].0 : b.count
        out.append(Data(b[s.1..<end]))
    }
    return out
}

// -------------------------------------------------------------- encoder ---

final class Encoder {
    let session: VTCompressionSession
    private let lock = NSLock()
    private(set) var aus: [Int: (Data, Bool)] = [:]
    private(set) var params: [Data] = []
    private(set) var failure: OSStatus = noErr

    init(width: Int, height: Int, fps: Double) throws {
        var s: VTCompressionSession?
        let st = VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height),
                                            codecType: kCMVideoCodecType_H264, encoderSpecification: nil,
                                            imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: nil,
                                            refcon: nil, compressionSessionOut: &s)
        guard st == noErr, let s else { throw Fail("VTCompressionSessionCreate: \(st)") }
        session = s
        func set(_ key: CFString, _ value: CFTypeRef) throws {
            let r = VTSessionSetProperty(s, key: key, value: value)
            if r != noErr { throw Fail("encoder property \(key): \(r)") }
        }
        // What the Vita decodes in hardware (the same profile and level as the NFS Most Wanted port).
        try set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_3_1)
        // No B-frames: the decoder hands out pictures in the order they go in, so frame k is the k-th picture.
        try set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        try set(kVTCompressionPropertyKey_RealTime, kCFBooleanFalse)
        try set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: 30))
        try set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 1.0))
        try set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: 3_000_000))
        try set(kVTCompressionPropertyKey_DataRateLimits, [NSNumber(value: 1_500_000), NSNumber(value: 1)] as CFArray)
        try set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        // pl_mpeg's pictures are BT.601 video range.
        try set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_SMPTE_C)
        try set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2)
        try set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4)
        VTCompressionSessionPrepareToEncodeFrames(s)
    }

    func encode(_ pb: CVPixelBuffer, index: Int, frameDuration: CMTime) {
        let pts = CMTimeMultiply(frameDuration, multiplier: Int32(index))
        let props: CFDictionary? = index == 0 ? [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue] as CFDictionary : nil
        let st = VTCompressionSessionEncodeFrame(session, imageBuffer: pb, presentationTimeStamp: pts, duration: frameDuration,
                                                 frameProperties: props, infoFlagsOut: nil) { [self] status, _, sbuf in
            lock.lock()
            defer { lock.unlock() }
            guard status == noErr, let sbuf else { failure = status == noErr ? -1 : status; return }
            let k = Int((CMSampleBufferGetPresentationTimeStamp(sbuf).value) / frameDuration.value)
            let (au, key, ps) = annexB(sbuf)
            if params.isEmpty { params = ps }
            aus[k] = (au, key)
        }
        if st != noErr { lock.lock(); failure = st; lock.unlock() }
    }

    func finish() {
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(session)
    }
}

/// One encoded frame as an Annex-B access unit (without the parameter sets), whether it is a keyframe, and the parameter sets.
func annexB(_ sbuf: CMSampleBuffer) -> (Data, Bool, [Data]) {
    var key = true
    if let atts = CMSampleBufferGetSampleAttachmentsArray(sbuf, createIfNecessary: false) as? [[CFString: Any]],
       let a = atts.first, let notSync = a[kCMSampleAttachmentKey_NotSync] as? Bool, notSync {
        key = false
    }
    let fd = CMSampleBufferGetFormatDescription(sbuf)!
    var count = 0
    var nalLen: Int32 = 0
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                       parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                       nalUnitHeaderLengthOut: &nalLen)
    var params: [Data] = []
    for i in 0..<count {
        var p: UnsafePointer<UInt8>?
        var n = 0
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &p,
                                                           parameterSetSizeOut: &n, parameterSetCountOut: nil,
                                                           nalUnitHeaderLengthOut: nil)
        params.append(Data(bytes: p!, count: n))
    }
    var out = Data()
    let bb = CMSampleBufferGetDataBuffer(sbuf)!
    let total = CMBlockBufferGetDataLength(bb)
    var raw = [UInt8](repeating: 0, count: total)
    CMBlockBufferCopyDataBytes(bb, atOffset: 0, dataLength: total, destination: &raw)
    var o = 0
    let h = Int(nalLen)
    while o + h <= total {
        var len = 0
        for k in 0..<h { len = (len << 8) | Int(raw[o + k]) }
        o += h
        out.append(contentsOf: startCode)
        out.append(contentsOf: raw[o..<min(o + len, total)])
        o += len
    }
    return (out, key, params)
}

// ------------------------------------------------------------- one movie ---

func le32(_ v: Int) -> [UInt8] { (0..<4).map { UInt8((v >> (8 * $0)) & 0xff) } }

func readLe32(_ d: Data, _ at: Int) -> Int {
    let s = d.startIndex + at
    return Int(d[s]) | Int(d[s + 1]) << 8 | Int(d[s + 2]) << 16 | Int(d[s + 3]) << 24
}

func fpsFraction(_ fps: Double) -> (Int, Int) {
    if abs(fps - 30000.0 / 1001.0) < 0.01 { return (30000, 1001) }
    if abs(fps - 24000.0 / 1001.0) < 0.01 { return (24000, 1001) }
    return (Int((fps * 1000).rounded()), 1000)
}

/// NV12 pixel buffer with a frame from pl_mpeg; false at the end of the movie.
func nextFrame(_ m: OpaquePointer, into pb: CVPixelBuffer) -> Bool {
    CVPixelBufferLockBaseAddress(pb, [])
    defer { CVPixelBufferUnlockBaseAddress(pb, []) }
    let y = CVPixelBufferGetBaseAddressOfPlane(pb, 0)!.assumingMemoryBound(to: UInt8.self)
    let uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1)!.assumingMemoryBound(to: UInt8.self)
    return pm_next_nv12(m, y, Int32(CVPixelBufferGetBytesPerRowOfPlane(pb, 0)), uv,
                        Int32(CVPixelBufferGetBytesPerRowOfPlane(pb, 1))) != 0
}

func makeNV12(_ w: Int, _ h: Int) throws -> CVPixelBuffer {
    var pb: CVPixelBuffer?
    let attrs = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary] as CFDictionary
    let st = CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &pb)
    guard st == kCVReturnSuccess, let pb else { throw Fail("CVPixelBufferCreate: \(st)") }
    CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, .shouldPropagate)
    CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_SMPTE_C, .shouldPropagate)
    CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, .shouldPropagate)
    return pb
}

struct Converted { var frames = 0, bytes = 0, keyframes = 0, maxAu = 0; var sps = SpsInfo(); var w = 0, h = 0; var fps = 0.0 }

func convert(dat: String, entry: Int, out: String) throws -> Converted {
    guard let m = pm_open(dat, Int32(entry)) else { throw Fail("pl_mpeg cannot open the movie") }
    defer { pm_close(m) }
    var w: Int32 = 0, h: Int32 = 0
    var fps = 0.0
    pm_size(m, &w, &h, &fps)
    let (num, den) = fpsFraction(fps)
    let frameDuration = CMTime(value: CMTimeValue(den), timescale: CMTimeScale(num))
    let enc = try Encoder(width: Int(w), height: Int(h), fps: fps)
    var n = 0
    while true {
        let pb = try makeNV12(Int(w), Int(h))
        if !nextFrame(m, into: pb) { break }
        enc.encode(pb, index: n, frameDuration: frameDuration)
        n += 1
        // Keeps only a few frames in flight.
        if n % 16 == 0 { VTCompressionSessionCompleteFrames(enc.session, untilPresentationTimeStamp: CMTimeMultiply(frameDuration, multiplier: Int32(n - 8))) }
    }
    enc.finish()
    guard enc.failure == noErr else { throw Fail("encoder: \(enc.failure)") }
    guard n > 0, enc.aus.count == n, !enc.params.isEmpty else { throw Fail("\(enc.aus.count) access units for \(n) frames") }
    var c = Converted(frames: n, w: Int(w), h: Int(h), fps: fps)
    guard enc.params.count == 2, enc.params[0].first! & 0x1f == 7, enc.params[1].first! & 0x1f == 8 else {
        throw Fail("expected one SPS and one PPS, got \(enc.params.count) parameter sets")
    }
    let sps = try spsWithoutReordering(enc.params[0])
    c.sps = try parseSps(sps)
    var head = Data()
    for p in [sps, enc.params[1]] { head.append(contentsOf: startCode); head.append(p) }
    let indexOffset = 64, dataOffset = 64 + 12 * n
    var index: [UInt8] = []
    var data = Data()
    for k in 0..<n {
        let (body, key) = enc.aus[k]!
        if k == 0 && !key { throw Fail("the first frame is not a keyframe") }
        let au = key ? head + body : body
        index += le32(dataOffset + data.count) + le32(au.count) + le32(key ? 1 : 0)
        data.append(au)
        c.maxAu = max(c.maxAu, au.count)
        if key { c.keyframes += 1 }
    }
    var file = Data("TMNTAVC1".utf8)
    file.append(contentsOf: le32(1) + le32(Int(w)) + le32(Int(h)) + le32(n) + le32(num) + le32(den) + le32(c.sps.maxRefFrames)
                 + le32(c.maxAu) + le32(indexOffset) + le32(dataOffset))
    file.append(contentsOf: [UInt8](repeating: 0, count: 64 - file.count))
    file.append(contentsOf: index)
    file.append(data)
    try file.write(to: URL(fileURLWithPath: out))
    c.bytes = file.count
    return c
}

// --------------------------------------------------------------- verify ---

/// Decodes the file back with VideoToolbox, in the order the Vita feeds it, and compares the luma with pl_mpeg's frames.
func verify(file: String, dat: String, entry: Int) throws -> (Int, Double, Double) {
    let d = try Data(contentsOf: URL(fileURLWithPath: file))
    guard d.prefix(8) == Data("TMNTAVC1".utf8) else { throw Fail("bad magic") }
    let w = readLe32(d, 12), h = readLe32(d, 16), n = readLe32(d, 20), indexOffset = readLe32(d, 40)
    func au(_ k: Int) -> Data {
        let off = readLe32(d, indexOffset + 12 * k), size = readLe32(d, indexOffset + 12 * k + 4)
        return d.subdata(in: off..<off + size)
    }
    let first = nalUnits(au(0))
    guard let sps = first.first(where: { $0.first! & 0x1f == 7 }), let pps = first.first(where: { $0.first! & 0x1f == 8 }) else {
        throw Fail("no SPS/PPS in the first frame")
    }
    var fd: CMVideoFormatDescription?
    let spsB = [UInt8](sps), ppsB = [UInt8](pps)
    let st = spsB.withUnsafeBufferPointer { s in
        ppsB.withUnsafeBufferPointer { p in
            let ptrs = [s.baseAddress!, p.baseAddress!]
            return CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator: nil, parameterSetCount: 2, parameterSetPointers: ptrs,
                                                                       parameterSetSizes: [spsB.count, ppsB.count],
                                                                       nalUnitHeaderLength: 4, formatDescriptionOut: &fd)
        }
    }
    guard st == noErr, let fd else { throw Fail("format description: \(st)") }
    var ds: VTDecompressionSession?
    let attrs = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange] as CFDictionary
    let st2 = VTDecompressionSessionCreate(allocator: nil, formatDescription: fd, decoderSpecification: nil,
                                           imageBufferAttributes: attrs, outputCallback: nil, decompressionSessionOut: &ds)
    guard st2 == noErr, let ds else { throw Fail("decompression session: \(st2)") }
    defer { VTDecompressionSessionInvalidate(ds) }
    guard let m = pm_open(dat, Int32(entry)) else { throw Fail("pl_mpeg cannot open the movie") }
    defer { pm_close(m) }
    let ref = try makeNV12(w, h)
    var minPsnr = 1000.0, sum = 0.0
    for k in 0..<n {
        guard nextFrame(m, into: ref) else { throw Fail("pl_mpeg has fewer frames (\(k)) than the file (\(n))") }
        // Annex-B to 4-byte lengths, without the parameter sets (they are in the format description).
        var avcc: [UInt8] = []
        for nal in nalUnits(au(k)) where ![7, 8, 9].contains(Int(nal.first! & 0x1f)) {
            avcc += [UInt8(nal.count >> 24 & 0xff), UInt8(nal.count >> 16 & 0xff), UInt8(nal.count >> 8 & 0xff), UInt8(nal.count & 0xff)]
            avcc += nal
        }
        var bb: CMBlockBuffer?
        CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: avcc.count, blockAllocator: nil,
                                           customBlockSource: nil, offsetToData: 0, dataLength: avcc.count, flags: 0, blockBufferOut: &bb)
        CMBlockBufferReplaceDataBytes(with: avcc, blockBuffer: bb!, offsetIntoDestination: 0, dataLength: avcc.count)
        var sb: CMSampleBuffer?
        var size = avcc.count
        CMSampleBufferCreateReady(allocator: nil, dataBuffer: bb, formatDescription: fd, sampleCount: 1, sampleTimingEntryCount: 0,
                                  sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sb)
        var decoded: CVImageBuffer?
        var status: OSStatus = noErr
        VTDecompressionSessionDecodeFrame(ds, sampleBuffer: sb!, flags: [], infoFlagsOut: nil) { s, _, img, _, _ in
            status = s
            decoded = img
        }
        VTDecompressionSessionWaitForAsynchronousFrames(ds)
        guard status == noErr, let img = decoded else { throw Fail("frame \(k) did not decode: \(status)") }
        // Luma PSNR against pl_mpeg's frame k.
        CVPixelBufferLockBaseAddress(img, .readOnly)
        CVPixelBufferLockBaseAddress(ref, .readOnly)
        let a = CVPixelBufferGetBaseAddressOfPlane(img, 0)!.assumingMemoryBound(to: UInt8.self)
        let b = CVPixelBufferGetBaseAddressOfPlane(ref, 0)!.assumingMemoryBound(to: UInt8.self)
        let sa = CVPixelBufferGetBytesPerRowOfPlane(img, 0), sb2 = CVPixelBufferGetBytesPerRowOfPlane(ref, 0)
        var se = 0.0
        for y in 0..<h {
            for x in 0..<w {
                let e = Double(Int(a[y * sa + x]) - Int(b[y * sb2 + x]))
                se += e * e
            }
        }
        // MOVCONV_DUMP=k: luma of frame k from both decoders as PGM (diagnostics).
        if let dump = ProcessInfo.processInfo.environment["MOVCONV_DUMP"], Int(dump) == k {
            for (tag, base, stride) in [("h264", a, sa), ("plm", b, sb2)] {
                var pgm = Data("P5 \(w) \(h) 255\n".utf8)
                for y in 0..<h { pgm.append(UnsafeBufferPointer(start: base + y * stride, count: w)) }
                try pgm.write(to: URL(fileURLWithPath: "\(file).\(k).\(tag).pgm"))
            }
        }
        CVPixelBufferUnlockBaseAddress(img, .readOnly)
        CVPixelBufferUnlockBaseAddress(ref, .readOnly)
        let mse = se / Double(w * h)
        let psnr = mse == 0 ? 99.0 : 10 * log10(255 * 255 / mse)
        minPsnr = min(minPsnr, psnr)
        sum += psnr
        if psnr < 33 && ProcessInfo.processInfo.environment["MOVCONV_LOW"] != nil { print(String(format: "  frame %d: %.1f dB", k, psnr)) }
    }
    if nextFrame(m, into: ref) { throw Fail("pl_mpeg has more frames than the file (\(n))") }
    return (n, minPsnr, sum / Double(n))
}

// ----------------------------------------------------------------- main ---

let args = CommandLine.arguments
guard args.count >= 3 else {
    print("usage: movconv <game dir> <out dir> [V00_00 ...]")
    exit(2)
}
let gameDir = args[1], outDir = args[2]
let only = Set(args.dropFirst(3))
try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
var totalBytes = 0, totalFrames = 0, failed = 0
for file in 0..<3 {
    let dat = "\(gameDir)/TMNT_V0\(file).DAT"
    let count = Int(pm_entry_count(dat))
    for entry in 0..<count {
        let name = String(format: "V%02d_%02d", file, entry)
        if !only.isEmpty && !only.contains(name) { continue }
        let out = "\(outDir)/TMNT_\(name).avc"
        do {
            let t0 = Date()
            let c = try convert(dat: dat, entry: entry, out: out)
            let (n, minP, avgP) = try verify(file: out, dat: dat, entry: entry)
            totalBytes += c.bytes
            totalFrames += c.frames
            print(String(format: "%@: %dx%d %.2f fps, %d frames (%.1f s), %d keyframes, %.1f MB, largest frame %d KB; "
                         + "SPS profile %d level %d refs %d poc %d reorder %d buffering %d; check %d frames, luma PSNR min %.1f avg %.1f dB (%.0f s)",
                         name, c.w, c.h, c.fps, c.frames, Double(c.frames) / c.fps, c.keyframes, Double(c.bytes) / 1e6, c.maxAu / 1024,
                         c.sps.profile, c.sps.level, c.sps.maxRefFrames, c.sps.pocType, c.sps.reorderFrames, c.sps.decFrameBuffering,
                         n, minP, avgP, Date().timeIntervalSince(t0)))
        } catch {
            failed += 1
            print("\(name): FAILED: \(error)")
        }
    }
}
print(String(format: "Done: %d frames, %.1f MB, %d failed", totalFrames, Double(totalBytes) / 1e6, failed))
exit(failed == 0 ? 0 : 1)
