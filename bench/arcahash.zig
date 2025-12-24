const std = @import("std");
const math = std.math;
const mem = std.mem;

pub const ArcaHash = struct {
    state: u64,
    seed: u64,

    const P1: u64 = 0xa0761d6478bd642f;
    const P2: u64 = 0xe7037ed1a0b428db;
    const P3: u64 = 0x8ebc6af09c88c6e3;
    const P4: u64 = 0x27d4eb2f165667c5;

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
        var h3 = self.state ^ P2;
        var h4 = self.seed ^ P3;

        // Process 128-byte blocks with 8 parallel lanes for maximum throughput
        while (len >= 128) : ({
            ptr += 128;
            len -= 128;
        }) {
            // Load 16 u64 values (128 bytes)
            const v1 = mem.readInt(u64, ptr[0..8], .little);
            const v2 = mem.readInt(u64, ptr[8..16], .little);
            const v3 = mem.readInt(u64, ptr[16..24], .little);
            const v4 = mem.readInt(u64, ptr[24..32], .little);
            const v5 = mem.readInt(u64, ptr[32..40], .little);
            const v6 = mem.readInt(u64, ptr[40..48], .little);
            const v7 = mem.readInt(u64, ptr[48..56], .little);
            const v8 = mem.readInt(u64, ptr[56..64], .little);
            const v9 = mem.readInt(u64, ptr[64..72], .little);
            const v10 = mem.readInt(u64, ptr[72..80], .little);
            const v11 = mem.readInt(u64, ptr[80..88], .little);
            const v12 = mem.readInt(u64, ptr[88..96], .little);
            const v13 = mem.readInt(u64, ptr[96..104], .little);
            const v14 = mem.readInt(u64, ptr[104..112], .little);
            const v15 = mem.readInt(u64, ptr[112..120], .little);
            const v16 = mem.readInt(u64, ptr[120..128], .little);

            // Mix with 4 independent accumulators - maximizes ILP
            h1 = h1 *% P1 ^ mix(v1, v2);
            h2 = h2 *% P2 ^ mix(v3, v4);
            h3 = h3 *% P3 ^ mix(v5, v6);
            h4 = h4 *% P4 ^ mix(v7, v8);

            h1 ^= mix(v9, v10);
            h2 ^= mix(v11, v12);
            h3 ^= mix(v13, v14);
            h4 ^= mix(v15, v16);
        }

        // Process 64-byte blocks
        while (len >= 64) : ({
            ptr += 64;
            len -= 64;
        }) {
            const v1 = mem.readInt(u64, ptr[0..8], .little);
            const v2 = mem.readInt(u64, ptr[8..16], .little);
            const v3 = mem.readInt(u64, ptr[16..24], .little);
            const v4 = mem.readInt(u64, ptr[24..32], .little);
            const v5 = mem.readInt(u64, ptr[32..40], .little);
            const v6 = mem.readInt(u64, ptr[40..48], .little);
            const v7 = mem.readInt(u64, ptr[48..56], .little);
            const v8 = mem.readInt(u64, ptr[56..64], .little);

            h1 ^= mix(v1, v2);
            h2 ^= mix(v3, v4);
            h3 ^= mix(v5, v6);
            h4 ^= mix(v7, v8);
        }

        // Process 32-byte blocks
        while (len >= 32) : ({
            ptr += 32;
            len -= 32;
        }) {
            const v1 = mem.readInt(u64, ptr[0..8], .little);
            const v2 = mem.readInt(u64, ptr[8..16], .little);
            const v3 = mem.readInt(u64, ptr[16..24], .little);
            const v4 = mem.readInt(u64, ptr[24..32], .little);

            h1 ^= mix(v1, v2);
            h2 ^= mix(v3, v4);
        }

        // Merge all 4 accumulators with avalanche
        var h = mix(h1, h2) ^ mix(h3, h4);
        h = mix(h, h1 ^ h3);

        // Process 8-byte blocks
        while (len >= 8) : ({
            ptr += 8;
            len -= 8;
        }) {
            const v = mem.readInt(u64, ptr[0..8], .little);
            h ^= mix(v, P1);
        }

        // Tail processing
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

    pub fn finalize(self: *const ArcaHash) u64 {
        var h = self.state;
        h ^= h >> 33;
        h = mix(h, P2);
        h ^= h >> 29;
        h = mix(h, P3);
        h ^= h >> 32;
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
    return state.finalize();
}

export fn arca_oneshot(data: [*]const u8, len: usize, seed: u64) u64 {
    return ArcaHash.hash(data[0..len], seed);
}