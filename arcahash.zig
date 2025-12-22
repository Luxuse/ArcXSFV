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
        @setRuntimeSafety(false); // Crucial pour le benchmark
        var len = input.len;
        var ptr = input.ptr;
        var h1 = self.state ^ @as(u64, @truncate(len));
        var h2 = self.seed ^ P2; // Deuxième accumulateur parallèle

        // --- Ultra Fast Path: 32-byte blocks ---
        // On traite 4x8 octets en même temps pour casser les dépendances
        while (len >= 32) : ({
            ptr += 32;
            len -= 32;
        }) {
            const v1 = mem.readInt(u64, ptr[0..8], .little);
            const v2 = mem.readInt(u64, ptr[8..16], .little);
            const v3 = mem.readInt(u64, ptr[16..24], .little);
            const v4 = mem.readInt(u64, ptr[24..32], .little);

            h1 = mix(v1 ^ P1, v2 ^ h1);
            h2 = mix(v3 ^ P2, v4 ^ h2);
        }

        // On fusionne les deux accumulateurs
        var h = h1 ^ h2;

        // --- Mid Path: 8-byte blocks ---
        while (len >= 8) : ({
            ptr += 8;
            len -= 8;
        }) {
            h = mix(mem.readInt(u64, ptr[0..8], .little) ^ P1, h ^ P2);
        }

        // --- Tail Path: 1-7 bytes ---
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
            h = mix(tail ^ P1, h ^ P3);
        }

        self.state = h;
    }

    pub fn finalize(self: *ArcaHash) u64 {
        return mix(self.state, self.state ^ P1);
    }
};

// Interface pour le C++ (C-ABI)
export fn arca_init(ctx: *ArcaHash, seed: u64) void {
    ctx.* = ArcaHash.init(seed);
}

export fn arca_update(ctx: *ArcaHash, input: [*]const u8, len: usize) void {
    // Transforme le pointeur C et la longueur en "slice" Zig
    ctx.update(input[0..len]);
}

export fn arca_finalize(ctx: *ArcaHash) u64 {
    return ctx.finalize();
}