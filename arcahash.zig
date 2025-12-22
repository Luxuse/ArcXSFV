const std = @import("std");
const math = std.math;
const mem = std.mem;

pub const ArcaHash = struct {
    state: u64,
    seed: u64,

    const P1: u64 = 0xa0761d6478bd642f;
    const P2: u64 = 0xe7037ed1a0b428db;
    const P3: u64 = 0x8ebc6af09c88c6e3;

    inline fn mix(a: u64, b: u64) u64 {
        const product = @as(u128, a) *% b;
        return @as(u64, @truncate(product)) ^ @as(u64, @truncate(product >> 64));
    }

    pub fn init(seed: u64) ArcaHash {
        return .{
            .state = seed ^ P1,
            .seed = seed,
        };
    }

    pub fn update(self: *ArcaHash, input: []const u8) void {
        @setRuntimeSafety(false);
        
        var len = input.len;
        var ptr = input.ptr;
        var h1 = self.state;
        var h2 = self.seed;

        // Process 32-byte blocks with 2 parallel accumulators
        while (len >= 32) : ({
            ptr += 32;
            len -= 32;
        }) {
            // Load all 4 values first
            const v1 = mem.readInt(u64, ptr[0..8], .little);
            const v2 = mem.readInt(u64, ptr[8..16], .little);
            const v3 = mem.readInt(u64, ptr[16..24], .little);
            const v4 = mem.readInt(u64, ptr[24..32], .little);

            // Mix in parallel - no dependencies between these two
            h1 ^= mix(v1, v2);
            h2 ^= mix(v3, v4);
        }

        // Merge the two accumulators
        var h = mix(h1, h2);

        // Process 8-byte blocks
        while (len >= 8) : ({
            ptr += 8;
            len -= 8;
        }) {
            const v = mem.readInt(u64, ptr[0..8], .little);
            h ^= mix(v, P1);
        }

        // Tail
        if (len > 0) {
            var tail: u64 = 0;
            if (len >= 4) {
                tail = @as(u64, mem.readInt(u32, ptr[0..4], .little));
                tail |= (@as(u64, mem.readInt(u16, ptr[len - 2 ..][0..2], .little)) << 32);
                tail |= (@as(u64, ptr[len - 1]) << 48);
            } else {
                tail = @as(u64, ptr[0]);
                tail |= (@as(u64, ptr[len / 2]) << 8);
                tail |= (@as(u64, ptr[len - 1]) << 16);
            }
            h ^= mix(tail, P3);
        }

        self.state = h;
    }

    pub fn finalize(self: *ArcaHash) u64 {
        var h = self.state;
        h ^= h >> 33;
        h = mix(h, P2);
        h ^= h >> 29;
        return h;
    }

    pub fn hash(input: []const u8, seed: u64) u64 {
        var hasher = init(seed);
        hasher.update(input);
        return hasher.finalize();
    }
};

// C ABI exports
export fn arca_init(ctx: *ArcaHash, seed: u64) void {
    ctx.* = ArcaHash.init(seed);
}



export fn arca_create(out: *ArcaHash, seed: u64) void {
    out.* = ArcaHash.init(seed);
}

export fn arca_update(state: *ArcaHash, data: [*]const u8, len: usize) void {
    state.update(data[0..len]);
}

export fn arca_finalize(state: *const ArcaHash) u64 {
    var h = state.*;
    return h.finalize();
}

export fn arca_oneshot(data: [*]const u8, len: usize, seed: u64) u64 {
    return ArcaHash.hash(data[0..len], seed);
}

