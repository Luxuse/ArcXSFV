const std = @import("std");
const ArcaHash = @import("arcahash.zig").ArcaHash;

pub fn main() !void {
    const allocator = std.heap.page_allocator;

    // Configuration : 512 Mo
    const data_size = 512 * 1024 * 1024; 
    const buffer = try allocator.alloc(u8, data_size);
    defer allocator.free(buffer);

    @memset(buffer, 0xAC);

    // On utilise std.debug.print pour éviter les erreurs de namespace Io
    std.debug.print("Benchmarking ArcaHash (MUM-mix version)...\n", .{});
    std.debug.print("Buffer size: {d} MB\n\n", .{data_size / 1024 / 1024});

    var timer = try std.time.Timer.start();
    const iterations = 10;
    var total_time: u64 = 0;
    var last_hash: u64 = 0;

    for (0..iterations) |_| {
        timer.reset();
        
        var h = ArcaHash.init(0);
        h.update(buffer);
        last_hash = h.finalize();

        total_time += timer.read();
    }

    const avg_ns = total_time / iterations;
    const avg_s = @as(f64, @floatFromInt(avg_ns)) / 1e9;
    const size_gb = @as(f64, @floatFromInt(data_size)) / 1024 / 1024 / 1024;
    const speed_gbs = size_gb / avg_s;

    std.debug.print("Results:\n", .{});
    std.debug.print("- Hash: 0x{x}\n", .{last_hash});
    std.debug.print("- Avg Time: {d:.4} ms\n", .{@as(f64, @floatFromInt(avg_ns)) / 1e6});
    std.debug.print("- Avg Speed: {d:.2} GB/s\n", .{speed_gbs});
    std.debug.print("---------------------------\n", .{});
}