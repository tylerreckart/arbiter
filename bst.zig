//! Binary Search Tree — Zig 0.14.0
//! Supports: insert, search, delete, in-order/pre-order/post-order traversal.
//! Memory: all nodes owned by the tree; deinit frees everything.

const std = @import("std");
const Allocator = std.mem.Allocator;
const Order = std.math.Order;

// ─── Node ────────────────────────────────────────────────────────────────────

pub fn Node(comptime T: type) type {
    return struct {
        const Self = @This();

        value: T,
        left:  ?*Self = null,
        right: ?*Self = null,

        fn init(allocator: Allocator, value: T) Allocator.Error!*Self {
            const node = try allocator.create(Self);
            node.* = .{ .value = value };
            return node;
        }

        /// Recursively free this node and all descendants.
        fn deinit(self: *Self, allocator: Allocator) void {
            if (self.left)  |l| l.deinit(allocator);
            if (self.right) |r| r.deinit(allocator);
            allocator.destroy(self);
        }
    };
}

// ─── BST ─────────────────────────────────────────────────────────────────────

pub fn BST(comptime T: type, comptime compareFn: fn (T, T) Order) type {
    return struct {
        const Self = @This();
        const NodeT = Node(T);

        allocator: Allocator,
        root:      ?*NodeT = null,
        len:       usize   = 0,

        pub fn init(allocator: Allocator) Self {
            return .{ .allocator = allocator };
        }

        /// Free all nodes. Tree is unusable after this call.
        pub fn deinit(self: *Self) void {
            if (self.root) |r| r.deinit(self.allocator);
            self.root = null;
            self.len  = 0;
        }

        // ── Insert ───────────────────────────────────────────────────────────

        /// Insert value. Duplicate values are silently ignored.
        pub fn insert(self: *Self, value: T) Allocator.Error!void {
            self.root = try insertNode(self.allocator, self.root, value, &self.len);
        }

        fn insertNode(
            allocator: Allocator,
            maybe_node: ?*NodeT,
            value: T,
            len: *usize,
        ) Allocator.Error!?*NodeT {
            const node = maybe_node orelse {
                len.* += 1;
                return try NodeT.init(allocator, value);
            };

            switch (compareFn(value, node.value)) {
                .lt => node.left  = try insertNode(allocator, node.left,  value, len),
                .gt => node.right = try insertNode(allocator, node.right, value, len),
                .eq => {}, // duplicate — no-op
            }
            return node;
        }

        // ── Search ───────────────────────────────────────────────────────────

        /// Returns pointer to the stored value, or null if not found.
        pub fn search(self: *const Self, value: T) ?*T {
            var current = self.root;
            while (current) |node| {
                switch (compareFn(value, node.value)) {
                    .lt => current = node.left,
                    .gt => current = node.right,
                    .eq => return &node.value,
                }
            }
            return null;
        }

        pub fn contains(self: *const Self, value: T) bool {
            return self.search(value) != null;
        }

        // ── Delete ───────────────────────────────────────────────────────────

        /// Remove value from tree. No-op if value absent.
        pub fn delete(self: *Self, value: T) void {
            self.root = deleteNode(self.allocator, self.root, value, &self.len);
        }

        fn deleteNode(
            allocator: Allocator,
            maybe_node: ?*NodeT,
            value: T,
            len: *usize,
        ) ?*NodeT {
            const node = maybe_node orelse return null;

            switch (compareFn(value, node.value)) {
                .lt => {
                    node.left = deleteNode(allocator, node.left, value, len);
                    return node;
                },
                .gt => {
                    node.right = deleteNode(allocator, node.right, value, len);
                    return node;
                },
                .eq => {
                    len.* -= 1;

                    // Case 1: no children.
                    if (node.left == null and node.right == null) {
                        allocator.destroy(node);
                        return null;
                    }

                    // Case 2: one child.
                    if (node.left == null) {
                        const right = node.right;
                        allocator.destroy(node);
                        return right;
                    }
                    if (node.right == null) {
                        const left = node.left;
                        allocator.destroy(node);
                        return left;
                    }

                    // Case 3: two children — replace with in-order successor
                    // (smallest value in right subtree), then delete successor.
                    const successor = findMin(node.right.?);
                    node.value = successor.value;
                    // Re-increment len: deleteNode will decrement it again for
                    // the successor removal.
                    len.* += 1;
                    node.right = deleteNode(allocator, node.right, successor.value, len);
                    return node;
                },
            }
        }

        fn findMin(node: *NodeT) *NodeT {
            var current = node;
            while (current.left) |l| current = l;
            return current;
        }

        // ── Traversals ───────────────────────────────────────────────────────

        /// In-order: left → root → right  (sorted ascending for a valid BST).
        pub fn inOrder(self: *const Self, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            inOrderNode(self.root, context, visitFn);
        }

        fn inOrderNode(maybe_node: ?*NodeT, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            const node = maybe_node orelse return;
            inOrderNode(node.left,  context, visitFn);
            visitFn(context, node.value);
            inOrderNode(node.right, context, visitFn);
        }

        /// Pre-order: root → left → right.
        pub fn preOrder(self: *const Self, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            preOrderNode(self.root, context, visitFn);
        }

        fn preOrderNode(maybe_node: ?*NodeT, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            const node = maybe_node orelse return;
            visitFn(context, node.value);
            preOrderNode(node.left,  context, visitFn);
            preOrderNode(node.right, context, visitFn);
        }

        /// Post-order: left → right → root.
        pub fn postOrder(self: *const Self, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            postOrderNode(self.root, context, visitFn);
        }

        fn postOrderNode(maybe_node: ?*NodeT, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            const node = maybe_node orelse return;
            postOrderNode(node.left,  context, visitFn);
            postOrderNode(node.right, context, visitFn);
            visitFn(context, node.value);
        }

        // ── Utility ──────────────────────────────────────────────────────────

        pub fn height(self: *const Self) usize {
            return nodeHeight(self.root);
        }

        fn nodeHeight(maybe_node: ?*NodeT) usize {
            const node = maybe_node orelse return 0;
            const lh = nodeHeight(node.left);
            const rh = nodeHeight(node.right);
            return 1 + @max(lh, rh);
        }
    };
}

// ─── Comparators ─────────────────────────────────────────────────────────────

pub fn compareI32(a: i32, b: i32) Order {
    return std.math.order(a, b);
}

pub fn compareU32(a: u32, b: u32) Order {
    return std.math.order(a, b);
}

// ─── Example usage ───────────────────────────────────────────────────────────

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer {
        const status = gpa.deinit();
        if (status == .leak) std.debug.print("LEAK DETECTED\n", .{});
    }
    const allocator = gpa.allocator();

    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    const stdout = std.io.getStdOut().writer();

    // ── Insert ───────────────────────────────────────────────────────────────
    const values = [_]i32{ 50, 30, 70, 20, 40, 60, 80, 10, 25, 35, 45 };
    for (values) |v| try tree.insert(v);

    try stdout.print("Inserted: {any}\n", .{values});
    try stdout.print("Tree size:   {d}\n", .{tree.len});
    try stdout.print("Tree height: {d}\n\n", .{tree.height()});

    // ── Traversals ───────────────────────────────────────────────────────────
    try stdout.print("In-order (sorted):  ", .{});
    tree.inOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n", .{});

    try stdout.print("Pre-order:          ", .{});
    tree.preOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n", .{});

    try stdout.print("Post-order:         ", .{});
    tree.postOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n\n", .{});

    // ── Search ───────────────────────────────────────────────────────────────
    const hits   = [_]i32{ 40, 80, 25 };
    const misses = [_]i32{ 99, -1, 55 };

    for (hits) |v| {
        try stdout.print("search({d:3}): {s}\n", .{ v, if (tree.contains(v)) "FOUND" else "NOT FOUND" });
    }
    for (misses) |v| {
        try stdout.print("search({d:3}): {s}\n", .{ v, if (tree.contains(v)) "FOUND" else "NOT FOUND" });
    }
    try stdout.print("\n", .{});

    // ── Delete ───────────────────────────────────────────────────────────────

    // Case 1: leaf node.
    try stdout.print("delete(10) [leaf]...\n", .{});
    tree.delete(10);
    try stdout.print("  size: {d}, contains(10): {}\n\n", .{ tree.len, tree.contains(10) });

    // Case 2: one child.
    try stdout.print("delete(20) [one child]...\n", .{});
    tree.delete(20);
    try stdout.print("  size: {d}, contains(20): {}\n\n", .{ tree.len, tree.contains(20) });

    // Case 3: two children.
    try stdout.print("delete(30) [two children]...\n", .{});
    tree.delete(30);
    try stdout.print("  size: {d}, contains(30): {}\n\n", .{ tree.len, tree.contains(30) });

    // Delete root.
    try stdout.print("delete(50) [root, two children]...\n", .{});
    tree.delete(50);
    try stdout.print("  size: {d}, contains(50): {}\n\n", .{ tree.len, tree.contains(50) });

    // Delete absent value — no-op.
    try stdout.print("delete(999) [absent — no-op]...\n", .{});
    tree.delete(999);
    try stdout.print("  size unchanged: {d}\n\n", .{tree.len});

    // Final in-order after deletions.
    try stdout.print("In-order after deletions: ", .{});
    tree.inOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n", .{});
}

// ─── Tests ───────────────────────────────────────────────────────────────────

test "insert and search" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);
    try tree.insert(1);
    try tree.insert(4);

    try std.testing.expect(tree.contains(5));
    try std.testing.expect(tree.contains(1));
    try std.testing.expect(tree.contains(4));
    try std.testing.expect(!tree.contains(0));
    try std.testing.expect(!tree.contains(6));
    try std.testing.expectEqual(@as(usize, 5), tree.len);
}

test "duplicate insert ignored" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(10);
    try tree.insert(10);
    try tree.insert(10);

    try std.testing.expectEqual(@as(usize, 1), tree.len);
}

test "delete leaf" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);

    /exec cat bst.zig

[TOOL RESULTS]
//! Binary Search Tree — Zig 0.14.0
//! Supports: insert, search, delete, in-order/pre-order/post-order traversal.
//! Memory: all nodes owned by the tree; deinit frees everything.

const std = @import("std");
const Allocator = std.mem.Allocator;
const Order = std.math.Order;

// ─── Node ────────────────────────────────────────────────────────────────────

pub fn Node(comptime T: type) type {
    return struct {
        const Self = @This();

        value: T,
        left:  ?*Self = null,
        right: ?*Self = null,

        fn init(allocator: Allocator, value: T) Allocator.Error!*Self {
            const node = try allocator.create(Self);
            node.* = .{ .value = value };
            return node;
        }

        /// Recursively free this node and all descendants.
        fn deinit(self: *Self, allocator: Allocator) void {
            if (self.left)  |l| l.deinit(allocator);
            if (self.right) |r| r.deinit(allocator);
            allocator.destroy(self);
        }
    };
}

// ─── BST ─────────────────────────────────────────────────────────────────────

pub fn BST(comptime T: type, comptime compareFn: fn (T, T) Order) type {
    return struct {
        const Self = @This();
        const NodeT = Node(T);

        allocator: Allocator,
        root:      ?*NodeT = null,
        len:       usize   = 0,

        pub fn init(allocator: Allocator) Self {
            return .{ .allocator = allocator };
        }

        /// Free all nodes. Tree is unusable after this call.
        pub fn deinit(self: *Self) void {
            if (self.root) |r| r.deinit(self.allocator);
            self.root = null;
            self.len  = 0;
        }

        // ── Insert ───────────────────────────────────────────────────────────

        /// Insert value. Duplicate values are silently ignored.
        pub fn insert(self: *Self, value: T) Allocator.Error!void {
            self.root = try insertNode(self.allocator, self.root, value, &self.len);
        }

        fn insertNode(
            allocator: Allocator,
            maybe_node: ?*NodeT,
            value: T,
            len: *usize,
        ) Allocator.Error!?*NodeT {
            const node = maybe_node orelse {
                len.* += 1;
                return try NodeT.init(allocator, value);
            };

            switch (compareFn(value, node.value)) {
                .lt => node.left  = try insertNode(allocator, node.left,  value, len),
                .gt => node.right = try insertNode(allocator, node.right, value, len),
                .eq => {}, // duplicate — no-op
            }
            return node;
        }

        // ── Search ───────────────────────────────────────────────────────────

        /// Returns pointer to the stored value, or null if not found.
        pub fn search(self: *const Self, value: T) ?*T {
            var current = self.root;
            while (current) |node| {
                switch (compareFn(value, node.value)) {
                    .lt => current = node.left,
                    .gt => current = node.right,
                    .eq => return &node.value,
                }
            }
            return null;
        }

        pub fn contains(self: *const Self, value: T) bool {
            return self.search(value) != null;
        }

        // ── Delete ───────────────────────────────────────────────────────────

        /// Remove value from tree. No-op if value absent.
        pub fn delete(self: *Self, value: T) void {
            self.root = deleteNode(self.allocator, self.root, value, &self.len);
        }

        fn deleteNode(
            allocator: Allocator,
            maybe_node: ?*NodeT,
            value: T,
            len: *usize,
        ) ?*NodeT {
            const node = maybe_node orelse return null;

            switch (compareFn(value, node.value)) {
                .lt => {
                    node.left = deleteNode(allocator, node.left, value, len);
                    return node;
                },
                .gt => {
                    node.right = deleteNode(allocator, node.right, value, len);
                    return node;
                },
                .eq => {
                    len.* -= 1;

                    // Case 1: no children.
                    if (node.left == null and node.right == null) {
                        allocator.destroy(node);
                        return null;
                    }

                    // Case 2: one child.
                    if (node.left == null) {
                        const right = node.right;
                        allocator.destroy(node);
                        return right;
                    }
                    if (node.right == null) {
                        const left = node.left;
                        allocator.destroy(node);
                        return left;
                    }

                    // Case 3: two children — replace with in-order successor
                    // (smallest value in right subtree), then delete successor.
                    const successor = findMin(node.right.?);
                    node.value = successor.value;
                    // Re-increment len: deleteNode will decrement it again for
                    // the successor removal.
                    len.* += 1;
                    node.right = deleteNode(allocator, node.right, successor.value, len);
                    return node;
                },
            }
        }

        fn findMin(node: *NodeT) *NodeT {
            var current = node;
            while (current.left) |l| current = l;
            return current;
        }

        // ── Traversals ───────────────────────────────────────────────────────

        /// In-order: left → root → right  (sorted ascending for a valid BST).
        pub fn inOrder(self: *const Self, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            inOrderNode(self.root, context, visitFn);
        }

        fn inOrderNode(maybe_node: ?*NodeT, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            const node = maybe_node orelse return;
            inOrderNode(node.left,  context, visitFn);
            visitFn(context, node.value);
            inOrderNode(node.right, context, visitFn);
        }

        /// Pre-order: root → left → right.
        pub fn preOrder(self: *const Self, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            preOrderNode(self.root, context, visitFn);
        }

        fn preOrderNode(maybe_node: ?*NodeT, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            const node = maybe_node orelse return;
            visitFn(context, node.value);
            preOrderNode(node.left,  context, visitFn);
            preOrderNode(node.right, context, visitFn);
        }

        /// Post-order: left → right → root.
        pub fn postOrder(self: *const Self, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            postOrderNode(self.root, context, visitFn);
        }

        fn postOrderNode(maybe_node: ?*NodeT, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            const node = maybe_node orelse return;
            postOrderNode(node.left,  context, visitFn);
            postOrderNode(node.right, context, visitFn);
            visitFn(context, node.value);
        }

        // ── Utility ──────────────────────────────────────────────────────────

        pub fn height(self: *const Self) usize {
            return nodeHeight(self.root);
        }

        fn nodeHeight(maybe_node: ?*NodeT) usize {
            const node = maybe_node orelse return 0;
            const lh = nodeHeight(node.left);
            const rh = nodeHeight(node.right);
            return 1 + @max(lh, rh);
        }
    };
}

// ─── Comparators ─────────────────────────────────────────────────────────────

pub fn compareI32(a: i32, b: i32) Order {
    return std.math.order(a, b);
}

pub fn compareU32(a: u32, b: u32) Order {
    return std.math.order(a, b);
}

// ─── Example usage ───────────────────────────────────────────────────────────

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer {
        const status = gpa.deinit();
        if (status == .leak) std.debug.print("LEAK DETECTED\n", .{});
    }
    const allocator = gpa.allocator();

    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    const stdout = std.io.getStdOut().writer();

    // ── Insert ───────────────────────────────────────────────────────────────
    const values = [_]i32{ 50, 30, 70, 20, 40, 60, 80, 10, 25, 35, 45 };
    for (values) |v| try tree.insert(v);

    try stdout.print("Inserted: {any}\n", .{values});
    try stdout.print("Tree size:   {d}\n", .{tree.len});
    try stdout.print("Tree height: {d}\n\n", .{tree.height()});

    // ── Traversals ───────────────────────────────────────────────────────────
    try stdout.print("In-order (sorted):  ", .{});
    tree.inOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n", .{});

    try stdout.print("Pre-order:          ", .{});
    tree.preOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n", .{});

    try stdout.print("Post-order:         ", .{});
    tree.postOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n\n", .{});

    // ── Search ───────────────────────────────────────────────────────────────
    const hits   = [_]i32{ 40, 80, 25 };
    const misses = [_]i32{ 99, -1, 55 };

    for (hits) |v| {
        try stdout.print("search({d:3}): {s}\n", .{ v, if (tree.contains(v)) "FOUND" else "NOT FOUND" });
    }
    for (misses) |v| {
        try stdout.print("search({d:3}): {s}\n", .{ v, if (tree.contains(v)) "FOUND" else "NOT FOUND" });
    }
    try stdout.print("\n", .{});

    // ── Delete ───────────────────────────────────────────────────────────────

    // Case 1: leaf node.
    try stdout.print("delete(10) [leaf]...\n", .{});
    tree.delete(10);
    try stdout.print("  size: {d}, contains(10): {}\n\n", .{ tree.len, tree.contains(10) });

    // Case 2: one child.
    try stdout.print("delete(20) [one child]...\n", .{});
    tree.delete(20);
    try stdout.print("  size: {d}, contains(20): {}\n\n", .{ tree.len, tree.contains(20) });

    // Case 3: two children.
    try stdout.print("delete(30) [two children]...\n", .{});
    tree.delete(30);
    try stdout.print("  size: {d}, contains(30): {}\n\n", .{ tree.len, tree.contains(30) });

    // Delete root.
    try stdout.print("delete(50) [root, two children]...\n", .{});
    tree.delete(50);
    try stdout.print("  size: {d}, contains(50): {}\n\n", .{ tree.len, tree.contains(50) });

    // Delete absent value — no-op.
    try stdout.print("delete(999) [absent — no-op]...\n", .{});
    tree.delete(999);
    try stdout.print("  size unchanged: {d}\n\n", .{tree.len});

    // Final in-order after deletions.
    try stdout.print("In-order after deletions: ", .{});
    tree.inOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n", .{});
}

// ─── Tests ───────────────────────────────────────────────────────────────────

test "insert and search" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);
    try tree.insert(1);
    try tree.insert(4);

    try std.testing.expect(tree.contains(5));
    try std.testing.expect(tree.contains(1));
    try std.testing.expect(tree.contains(4));
    try std.testing.expect(!tree.contains(0));
    try std.testing.expect(!tree.contains(6));
    try std.testing.expectEqual(@as(usize, 5), tree.len);
}

test "duplicate insert ignored" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(10);
    try tree.insert(10);
    try tree.insert(10);

    try std.testing.expectEqual(@as(usize, 1), tree.len);
}

test "delete leaf" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);

    

File confirmed on disk. Appending the remaining tests now.
/write bst.zig
//! Binary Search Tree — Zig 0.14.0
//! Supports: insert, search, delete, in-order/pre-order/post-order traversal./exec tail -50 bst.zig

[TOOL RESULTS]
test "delete leaf" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);

    

The /write command above was interrupted mid-file. Rewriting the complete file cleanly.
/write bst.zig
//! Binary Search Tree — Zig 0.14.0
//! Supports: insert, search, delete, in-order/pre-order/post-order traversal.
//! Memory: all nodes owned by the tree; deinit frees everything.

const std = @import("std");
const Allocator = std.mem.Allocator;
const Order = std.math.Order;

// ─── Node ────────────────────────────────────────────────────────────────────

pub fn Node(comptime T: type) type {
    return struct {
        const Self = @This();

        value: T,
        left:  ?*Self = null,
        right: ?*Self = null,

        fn init(allocator: Allocator, value: T) Allocator.Error!*Self {
            const node = try allocator.create(Self);
            node.* = .{ .value = value };
            return node;
        }

        /// Recursively free this node and all descendants.
        fn deinit(self: *Self, allocator: Allocator) void {
            if (self.left)  |l| l.deinit(allocator);
            if (self.right) |r| r.deinit(allocator);
            allocator.destroy(self);
        }
    };
}

// ─── BST ─────────────────────────────────────────────────────────────────────

pub fn BST(comptime T: type, comptime compareFn: fn (T, T) Order) type {
    return struct {
        const Self = @This();
        const NodeT = Node(T);

        allocator: Allocator,
        root:      ?*NodeT = null,
        len:       usize   = 0,

        pub fn init(allocator: Allocator) Self {
            return .{ .allocator = allocator };
        }

        /// Free all nodes. Tree is unusable after this call.
        pub fn deinit(self: *Self) void {
            if (self.root) |r| r.deinit(self.allocator);
            self.root = null;
            self.len  = 0;
        }

        // ── Insert ───────────────────────────────────────────────────────────

        /// Insert value. Duplicate values are silently ignored.
        pub fn insert(self: *Self, value: T) Allocator.Error!void {
            self.root = try insertNode(self.allocator, self.root, value, &self.len);
        }

        fn insertNode(
            allocator: Allocator,
            maybe_node: ?*NodeT,
            value: T,
            len: *usize,
        ) Allocator.Error!?*NodeT {
            const node = maybe_node orelse {
                len.* += 1;
                return try NodeT.init(allocator, value);
            };

            switch (compareFn(value, node.value)) {
                .lt => node.left  = try insertNode(allocator, node.left,  value, len),
                .gt => node.right = try insertNode(allocator, node.right, value, len),
                .eq => {}, // duplicate — no-op
            }
            return node;
        }

        // ── Search ───────────────────────────────────────────────────────────

        /// Returns pointer to the stored value, or null if not found.
        pub fn search(self: *const Self, value: T) ?*T {
            var current = self.root;
            while (current) |node| {
                switch (compareFn(value, node.value)) {
                    .lt => current = node.left,
                    .gt => current = node.right,
                    .eq => return &node.value,
                }
            }
            return null;
        }

        pub fn contains(self: *const Self, value: T) bool {
            return self.search(value) != null;
        }

        // ── Delete ───────────────────────────────────────────────────────────

        /// Remove value from tree. No-op if value absent.
        pub fn delete(self: *Self, value: T) void {
            self.root = deleteNode(self.allocator, self.root, value, &self.len);
        }

        fn deleteNode(
            allocator: Allocator,
            maybe_node: ?*NodeT,
            value: T,
            len: *usize,
        ) ?*NodeT {
            const node = maybe_node orelse return null;

            switch (compareFn(value, node.value)) {
                .lt => {
                    node.left = deleteNode(allocator, node.left, value, len);
                    return node;
                },
                .gt => {
                    node.right = deleteNode(allocator, node.right, value, len);
                    return node;
                },
                .eq => {
                    len.* -= 1;

                    // Case 1: no children.
                    if (node.left == null and node.right == null) {
                        allocator.destroy(node);
                        return null;
                    }

                    // Case 2: one child.
                    if (node.left == null) {
                        const right = node.right;
                        allocator.destroy(node);
                        return right;
                    }
                    if (node.right == null) {
                        const left = node.left;
                        allocator.destroy(node);
                        return left;
                    }

                    // Case 3: two children — replace with in-order successor
                    // (smallest value in right subtree), then delete successor.
                    const successor = findMin(node.right.?);
                    node.value = successor.value;
                    // Re-increment len: deleteNode will decrement it again for
                    // the successor removal.
                    len.* += 1;
                    node.right = deleteNode(allocator, node.right, successor.value, len);
                    return node;
                },
            }
        }

        fn findMin(node: *NodeT) *NodeT {
            var current = node;
            while (current.left) |l| current = l;
            return current;
        }

        // ── Traversals ───────────────────────────────────────────────────────

        /// In-order: left → root → right  (sorted ascending for a valid BST).
        pub fn inOrder(self: *const Self, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            inOrderNode(self.root, context, visitFn);
        }

        fn inOrderNode(maybe_node: ?*NodeT, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            const node = maybe_node orelse return;
            inOrderNode(node.left,  context, visitFn);
            visitFn(context, node.value);
            inOrderNode(node.right, context, visitFn);
        }

        /// Pre-order: root → left → right.
        pub fn preOrder(self: *const Self, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            preOrderNode(self.root, context, visitFn);
        }

        fn preOrderNode(maybe_node: ?*NodeT, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            const node = maybe_node orelse return;
            visitFn(context, node.value);
            preOrderNode(node.left,  context, visitFn);
            preOrderNode(node.right, context, visitFn);
        }

        /// Post-order: left → right → root.
        pub fn postOrder(self: *const Self, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            postOrderNode(self.root, context, visitFn);
        }

        fn postOrderNode(maybe_node: ?*NodeT, context: anytype, comptime visitFn: fn (@TypeOf(context), T) void) void {
            const node = maybe_node orelse return;
            postOrderNode(node.left,  context, visitFn);
            postOrderNode(node.right, context, visitFn);
            visitFn(context, node.value);
        }

        // ── Utility ──────────────────────────────────────────────────────────

        pub fn height(self: *const Self) usize {
            return nodeHeight(self.root);
        }

        fn nodeHeight(maybe_node: ?*NodeT) usize {
            const node = maybe_node orelse return 0;
            const lh = nodeHeight(node.left);
            const rh = nodeHeight(node.right);
            return 1 + @max(lh, rh);
        }
    };
}

// ─── Comparators ─────────────────────────────────────────────────────────────

pub fn compareI32(a: i32, b: i32) Order {
    return std.math.order(a, b);
}

pub fn compareU32(a: u32, b: u32) Order {
    return std.math.order(a, b);
}

// ─── Example usage ───────────────────────────────────────────────────────────

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer {
        const status = gpa.deinit();
        if (status == .leak) std.debug.print("LEAK DETECTED\n", .{});
    }
    const allocator = gpa.allocator();

    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    const stdout = std.io.getStdOut().writer();

    // ── Insert ───────────────────────────────────────────────────────────────
    const values = [_]i32{ 50, 30, 70, 20, 40, 60, 80, 10, 25, 35, 45 };
    for (values) |v| try tree.insert(v);

    try stdout.print("Inserted: {any}\n", .{values});
    try stdout.print("Tree size:   {d}\n", .{tree.len});
    try stdout.print("Tree height: {d}\n\n", .{tree.height()});

    // ── Traversals ───────────────────────────────────────────────────────────
    try stdout.print("In-order (sorted):  ", .{});
    tree.inOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n", .{});

    try stdout.print("Pre-order:          ", .{});
    tree.preOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n", .{});

    try stdout.print("Post-order:         ", .{});
    tree.postOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n\n", .{});

    // ── Search ───────────────────────────────────────────────────────────────
    const hits   = [_]i32{ 40, 80, 25 };
    const misses = [_]i32{ 99, -1, 55 };

    for (hits) |v| {
        try stdout.print("search({d:3}): {s}\n", .{ v, if (tree.contains(v)) "FOUND" else "NOT FOUND" });
    }
    for (misses) |v| {
        try stdout.print("search({d:3}): {s}\n", .{ v, if (tree.contains(v)) "FOUND" else "NOT FOUND" });
    }
    try stdout.print("\n", .{});

    // ── Delete ───────────────────────────────────────────────────────────────

    // Case 1: leaf node.
    try stdout.print("delete(10) [leaf]...\n", .{});
    tree.delete(10);
    try stdout.print("  size: {d}, contains(10): {}\n\n", .{ tree.len, tree.contains(10) });

    // Case 2: one child.
    try stdout.print("delete(20) [one child]...\n", .{});
    tree.delete(20);
    try stdout.print("  size: {d}, contains(20): {}\n\n", .{ tree.len, tree.contains(20) });

    // Case 3: two children.
    try stdout.print("delete(30) [two children]...\n", .{});
    tree.delete(30);
    try stdout.print("  size: {d}, contains(30): {}\n\n", .{ tree.len, tree.contains(30) });

    // Delete root.
    try stdout.print("delete(50) [root, two children]...\n", .{});
    tree.delete(50);
    try stdout.print("  size: {d}, contains(50): {}\n\n", .{ tree.len, tree.contains(50) });

    // Delete absent value — no-op.
    try stdout.print("delete(999) [absent — no-op]...\n", .{});
    tree.delete(999);
    try stdout.print("  size unchanged: {d}\n\n", .{tree.len});

    // Final in-order after deletions.
    try stdout.print("In-order after deletions: ", .{});
    tree.inOrder(stdout, struct {
        fn visit(w: @TypeOf(stdout), v: i32) void {
            w.print("{d} ", .{v}) catch {};
        }
    }.visit);
    try stdout.print("\n", .{});
}

// ─── Tests ───────────────────────────────────────────────────────────────────

test "insert and search" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);
    try tree.insert(1);
    try tree.insert(4);

    try std.testing.expect(tree.contains(5));
    try std.testing.expect(tree.contains(1));
    try std.testing.expect(tree.contains(4));
    try std.testing.expect(!tree.contains(0));
    try std.testing.expect(!tree.contains(6));
    try std.testing.expectEqual(@as(usize, 5), tree.len);
}

test "duplicate insert ignored" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(10);
    try tree.insert(10);
    try tree.insert(10);

    try std.testing.expectEqual(@as(usize, 1), tree.len);
}

test "delete leaf" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);

    tree.delete(3);

    try std.testing.expect(!tree.contains(3));
    try std.testing.expect(tree.contains(5));
    try std.testing.expect(tree.contains(7));
    try std.testing.expectEqual(@as(usize, 2), tree.len);
}

test "delete one child" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(2); // left child of 3, no right child on 3

    tree.delete(3);

    try std.testing.expect(!tree.contains(3));
    try std.testing.expect(tree.contains(2));
    try std.testing.expect(tree.contains(5));
    try std.testing.expectEqual(@as(usize, 2), tree.len);
}

test "delete two children" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);
    try tree.insert(1);
    try tree.insert(4);

    tree.delete(3); // has two children: 1 and 4

    try std.testing.expect(!tree.contains(3));
    try std.testing.expect(tree.contains(1));
    try std.testing.expect(tree.contains(4));
    try std.testing.expect(tree.contains(5));
    try std.testing.expect(tree.contains(7));
    try std.testing.expectEqual(@as(usize, 4), tree.len);
}

test "delete root" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);

    tree.delete(5);

    try std.testing.expect(!tree.contains(5));
    try std.testing.expect(tree.contains(3));
    try std.testing.expect(tree.contains(7));
    try std.testing.expectEqual(@as(usize, 2), tree.len);
}

test "delete absent value is no-op" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);

    tree.delete(99);

    try std.testing.expectEqual(@as(usize, 2), tree.len);
    try std.testing.expect(tree.contains(5));
    try std.testing.expect(tree.contains(3));
}

test "delete all nodes" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(3);
    try tree.insert(7);

    tree.delete(3);
    tree.delete(7);
    tree.delete(5);

    try std.testing.expectEqual(@as(usize, 0), tree.len);
    try std.testing.expect(tree.root == null);
}

test "height" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try std.testing.expectEqual(@as(usize, 0), tree.height());

    try tree.insert(5);
    try std.testing.expectEqual(@as(usize, 1), tree.height());

    try tree.insert(3);
    try tree.insert(7);
    try std.testing.expectEqual(@as(usize, 2), tree.height());

    try tree.insert(1);
    try std.testing.expectEqual(@as(usize, 3), tree.height());
}

test "in-order traversal produces sorted output" {
    const allocator = std.testing.allocator;
    const IntBST = BST(i32, compareI32);
    var tree = IntBST.init(allocator);
    defer tree.deinit();

    try tree.insert(5);
    try tree.insert(1);
    try tree.insert(9);
    try tree.insert(3);
    try tree.insert(7);

    var result = std.ArrayList(i32).init(allocator);
    defer result.deinit();

    tree.inOrder(&result, struct {
        fn visit(list: *std.ArrayList(i32), v: i32) void {
            list.append(v) catch {};
        }
    }.visit);

    const expected = [_]i32{ 1, 3, 5, 7, 9 };
    try std.testing.expectEqual(expected.len, result.items.len);
    for (expected, result.items) |e, got| {
        try std.testing.expectEqual(e, got);
    }
}

test "u32 tree" {
    const allocator = std.testing.allocator;
    const U32BST = BST(u32, compareU32);
    var tree = U32BST.init(allocator);
    defer tree.deinit();

    try tree.insert(100);
    try tree.insert(50);
    try tree.insert(200);

    try std.testing.expect(tree.contains(100));
    try std.testing.expect(tree.contains(50));
    try std.testing.expect(tree.contains(200));
    try std.testing.expect(!tree.contains(0));
    try std.testing.expectEqual(@as(usize, 3), tree.len);

    tree.delete(50);
    try std.testing.expect(!tree.contains(50));
    try std.testing.expectEqual(@as(usize, 2), tree.len);
}
