const std = @import("std");
const mem = std.mem;
const builtin = @import("builtin");

pub const ArcaHash = struct {
    state: u64,
    seed: u64,

    const P1: u64 = 0xa0761d6478bd642f;
    const P2: u64 = 0xe7037ed1a0b428db;
    const P3: u64 = 0x8ebc6af09c88c6e3;
    const P4: u64 = 0x27d4eb2f165667c5;

    const has_simd = switch (builtin.cpu.arch) {
        .x86_64 => std.Target.x86.featureSetHas(builtin.cpu.features, .sse2),
        .aarch64 => true,
        else => false,
    };

    inline fn mix(a: u64, b: u64) u64 {
        const product = @as(u128, a) *% b;
        return @as(u64, @truncate(product)) ^ @as(u64, @truncate(product >> 64));
    }

    /// SIMD mix for 4 parallel lanes
    inline fn mixVec4(a: @Vector(4, u64), b: @Vector(4, u64)) @Vector(4, u64) {
        // Emulate 128-bit multiply by doing 64x64->128 for each lane
        var result: @Vector(4, u64) = undefined;
        inline for (0..4) |i| {
            result[i] = mix(a[i], b[i]);
        }
        return result;
    }

    inline fn read64(ptr: [*]const u8) u64 {
        return @as(*align(1) const u64, @ptrCast(ptr)).*;
    }

    inline fn read32(ptr: [*]const u8) u32 {
        return @as(*align(1) const u32, @ptrCast(ptr)).*;
    }

    inline fn read16(ptr: [*]const u8) u16 {
        return @as(*align(1) const u16, @ptrCast(ptr)).*;
    }

    pub fn init(seed: u64) ArcaHash {
        return .{
            .state = seed ^ P1,
            .seed = seed,
        };
    }

    /// Ultra-wide SIMD path - process 4 hash states in parallel
    fn updateSIMD(self: *ArcaHash, input: []const u8) void {
        @setRuntimeSafety(false);

        var ptr = input.ptr;
        var len = input.len;

        const Vec4u64 = @Vector(4, u64);

        // Initialize 4 parallel hash states
        var acc1 = Vec4u64{ self.state, self.seed, self.state ^ P2, self.seed ^ P3 };
        var acc2 = Vec4u64{ P1, P2, P3, P4 };

        // Process 512-byte mega-blocks (128 bytes per accumulator lane)
        while (len >= 512) {
            @prefetch(ptr + 640, .{ .rw = .read, .locality = 3, .cache = .data });

            // Load 4x 16-byte chunks (64 bytes total)
            const chunk1 = @as(*align(1) const Vec4u64, @ptrCast(ptr)).*;
            const chunk2 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 32)).*;
            const chunk3 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 64)).*;
            const chunk4 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 96)).*;

            // Parallel mix operations
            acc1 = acc1 *% Vec4u64{ P1, P2, P3, P4 } ^ chunk1;
            acc2 ^= chunk2;
            acc1 ^= chunk3;
            acc2 = acc2 *% Vec4u64{ P2, P3, P4, P1 } ^ chunk4;

            // Second 128-byte stripe
            const chunk5 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 128)).*;
            const chunk6 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 160)).*;
            const chunk7 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 192)).*;
            const chunk8 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 224)).*;

            acc1 ^= chunk5;
            acc2 = acc2 *% Vec4u64{ P3, P4, P1, P2 } ^ chunk6;
            acc1 = acc1 *% Vec4u64{ P4, P1, P2, P3 } ^ chunk7;
            acc2 ^= chunk8;

            // Third 128-byte stripe
            const chunk9 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 256)).*;
            const chunk10 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 288)).*;
            const chunk11 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 320)).*;
            const chunk12 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 352)).*;

            acc1 = acc1 *% Vec4u64{ P1, P2, P3, P4 } ^ chunk9;
            acc2 ^= chunk10;
            acc1 ^= chunk11;
            acc2 = acc2 *% Vec4u64{ P2, P3, P4, P1 } ^ chunk12;

            // Fourth 128-byte stripe
            const chunk13 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 384)).*;
            const chunk14 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 416)).*;
            const chunk15 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 448)).*;
            const chunk16 = @as(*align(1) const Vec4u64, @ptrCast(ptr + 480)).*;

            acc1 ^= chunk13;
            acc2 = acc2 *% Vec4u64{ P3, P4, P1, P2 } ^ chunk14;
            acc1 = acc1 *% Vec4u64{ P4, P1, P2, P3 } ^ chunk15;
            acc2 ^= chunk16;

            ptr += 512;
            len -= 512;
        }

        // Merge accumulators
        const h1 = acc1[0] ^ acc1[1] ^ acc1[2] ^ acc1[3];
        const h2 = acc2[0] ^ acc2[1] ^ acc2[2] ^ acc2[3];

        self.state = mix(h1, h2);
        self.seed = mix(h2, h1);

        // Process remainder with scalar
        if (len > 0) {
            self.updateScalar(ptr[0..len], self.state, self.seed, self.state ^ P2, self.seed ^ P3);
        }
    }

    fn updateScalar(self: *ArcaHash, input: []const u8, h1_in: u64, h2_in: u64, h3_in: u64, h4_in: u64) void {
        @setRuntimeSafety(false);

        var ptr = input.ptr;
        var len = input.len;
        var h1 = h1_in;
        var h2 = h2_in;
        var h3 = h3_in;
        var h4 = h4_in;

        // Process 128-byte blocks
        while (len >= 128) {
            @prefetch(ptr + 192, .{ .rw = .read, .locality = 3, .cache = .data });

            h1 = h1 *% P1 ^ mix(read64(ptr), read64(ptr + 8));
            h2 = h2 *% P2 ^ mix(read64(ptr + 16), read64(ptr + 24));
            h3 = h3 *% P3 ^ mix(read64(ptr + 32), read64(ptr + 40));
            h4 = h4 *% P4 ^ mix(read64(ptr + 48), read64(ptr + 56));

            h1 ^= mix(read64(ptr + 64), read64(ptr + 72));
            h2 ^= mix(read64(ptr + 80), read64(ptr + 88));
            h3 ^= mix(read64(ptr + 96), read64(ptr + 104));
            h4 ^= mix(read64(ptr + 112), read64(ptr + 120));

            ptr += 128;
            len -= 128;
        }

        // Process 64-byte blocks
        if (len >= 64) {
            h1 ^= mix(read64(ptr), read64(ptr + 8));
            h2 ^= mix(read64(ptr + 16), read64(ptr + 24));
            h3 ^= mix(read64(ptr + 32), read64(ptr + 40));
            h4 ^= mix(read64(ptr + 48), read64(ptr + 56));
            ptr += 64;
            len -= 64;
        }

        // Process 32-byte blocks
        if (len >= 32) {
            h1 ^= mix(read64(ptr), read64(ptr + 8));
            h2 ^= mix(read64(ptr + 16), read64(ptr + 24));
            ptr += 32;
            len -= 32;
        }

        // Avalanche merge
        var h = mix(h1, h2) ^ mix(h3, h4);
        h = mix(h, h1 ^ h3);

        // Process remaining 8-byte blocks
        while (len >= 8) {
            h ^= mix(read64(ptr), P1);
            ptr += 8;
            len -= 8;
        }

        // Tail processing
        if (len > 0) {
            var tail: u64 = 0;
            if (len >= 4) {
                tail = @as(u64, read32(ptr));
                const tail_ptr = ptr + len;
                tail |= (@as(u64, read16(tail_ptr - 4)) << 32);
                tail |= (@as(u64, (tail_ptr - 1)[0]) << 56);
            } else {
                tail = @as(u64, ptr[0]);
                tail |= (@as(u64, ptr[len >> 1]) << 8);
                tail |= (@as(u64, ptr[len - 1]) << 16);
            }
            h ^= mix(tail, P3);
        }

        self.state = h;
    }

    pub fn update(self: *ArcaHash, input: []const u8) void {
        if (has_simd and input.len >= 512) {
            self.updateSIMD(input);
        } else {
            const h1 = self.state;
            const h2 = self.seed;
            const h3 = self.state ^ P2;
            const h4 = self.seed ^ P3;
            self.updateScalar(input, h1, h2, h3, h4);
        }
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
