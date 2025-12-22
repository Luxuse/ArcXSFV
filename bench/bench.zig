const std = @import("std");
const ArcaHash = @import("arcahash.zig").ArcaHash;

pub fn main() !void {
    const allocator = std.heap.page_allocator;
    
    // Multiple test sizes for comprehensive benchmarking
    const test_sizes = [_]usize{
        128 * 1024 * 1024,
        512 * 1024 * 1024,    // 512 MB
    };
    

    std.debug.print(" Benchmark Suite \n", .{});

    
    for (test_sizes) |size| {
        try benchmarkSize(allocator, size);
    }
    
    std.debug.print("\n✓ Benchmark complete!\n", .{});
}

fn benchmarkSize(allocator: std.mem.Allocator, data_size: usize) !void {
    const buffer = try allocator.alloc(u8, data_size);
    defer allocator.free(buffer);
    
    // Fill with pseudo-random data for more realistic testing
    var prng = std.Random.DefaultPrng.init(0xDEADBEEF);
    const random = prng.random();
    random.bytes(buffer);
    
    const size_mb = @as(f64, @floatFromInt(data_size)) / (1024 * 1024);
    std.debug.print("Testing {d:.1} MB \n", .{size_mb});
    
    var timer = try std.time.Timer.start();
    const iterations: usize = if (data_size <= 16 * 1024 * 1024) 100 else 10;
    
    // Warmup run
    {
        var h = ArcaHash.init(0);
        h.update(buffer);
        _ = h.finalize();
    }
    
    // Actual benchmark
    var total_time: u64 = 0;
    var min_time: u64 = std.math.maxInt(u64);
    var max_time: u64 = 0;
    var last_hash: u64 = 0;
    
    for (0..iterations) |_| {
        timer.reset();
        
        var h = ArcaHash.init(0);
        h.update(buffer);
        last_hash = h.finalize();
        
        const elapsed = timer.read();
        total_time += elapsed;
        min_time = @min(min_time, elapsed);
        max_time = @max(max_time, elapsed);
    }
    
    const avg_ns = total_time / iterations;
    const avg_s = @as(f64, @floatFromInt(avg_ns)) / 1e9;
    const min_s = @as(f64, @floatFromInt(min_time)) / 1e9;
    
    const size_gb = @as(f64, @floatFromInt(data_size)) / (1024 * 1024 * 1024);
    const avg_speed_gbs = size_gb / avg_s;
    const best_speed_gbs = size_gb / min_s;
    
    // Calculate bytes per cycle (assuming 4.5 GHz CPU)
    const cpu_ghz: f64 = 4.5;
    const cycles = @as(f64, @floatFromInt(avg_ns)) * cpu_ghz;
    const bytes_per_cycle = @as(f64, @floatFromInt(data_size)) / cycles;
    
    std.debug.print(" Hash:       0x{x:0>16}\n", .{last_hash});
    std.debug.print(" Iterations: {d}\n", .{iterations});
    std.debug.print("\n", .{});
    std.debug.print(" Avg Time:   {d:.3} ms\n", .{@as(f64, @floatFromInt(avg_ns)) / 1e6});
    std.debug.print(" Min Time:   {d:.3} ms\n", .{@as(f64, @floatFromInt(min_time)) / 1e6});
    std.debug.print(" Max Time:   {d:.3} ms\n", .{@as(f64, @floatFromInt(max_time)) / 1e6});
    std.debug.print("\n", .{});
    std.debug.print(" Avg Speed:  {d:.2} GB/s\n", .{avg_speed_gbs});
    std.debug.print("Peak Speed: {d:.2} GB/s\n", .{best_speed_gbs});
    std.debug.print("B/cycle:    {d:.2} @ {d:.1}GHz\n", .{bytes_per_cycle, cpu_ghz});
    std.debug.print("\n\n", .{});
}

test "benchmark comparison" {
    const data = "The quick brown fox jumps over the lazy dog";
    
    var timer = try std.time.Timer.start();
    const iterations = 1_000_000;
    
    timer.reset();
    for (0..iterations) |_| {
        const h = ArcaHash.hash(data, 0);
        std.mem.doNotOptimizeAway(h);
    }
    const elapsed = timer.read();
    
    const ns_per_hash = @as(f64, @floatFromInt(elapsed)) / @as(f64, iterations);
    std.debug.print("Small string (44 bytes): {d:.2} ns/hash\n", .{ns_per_hash});
}