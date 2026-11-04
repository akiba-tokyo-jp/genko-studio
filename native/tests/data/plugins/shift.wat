;; A Genko test module of PhotoCraft's plug-in ABI v1 (PC5 J-f, docs/PLUGIN_WASM.md): its manifest at 16, a bump
;; allocator on memory.grow (each band gets a fresh instance), and pc_filter as below. Made into .wasm once with
;; `cargo run --example wat2wasm` of native/wasm-host (native/tests/data/plugins/MANIFEST.json).
(module
  
  (memory (export "memory") 1)
  (data (i32.const 16) "{\22id\22:\22test.shift\22,\22name\22:\22Shift\22,\22version\22:\221\22,\22kind\22:\22filter\22,\22overlap\22:1}")
  (func (export "pc_abi_version") (result i32) (i32.const 1))
  (func (export "pc_manifest") (result i64)
    (i64.or (i64.shl (i64.const 76) (i64.const 32)) (i64.const 16)))
  (func (export "pc_alloc") (param $n i32) (result i32) (local $old i32)
    (local.set $old (memory.grow (i32.shr_u (i32.add (local.get $n) (i32.const 65535)) (i32.const 16))))
    (if (result i32) (i32.eq (local.get $old) (i32.const -1))
      (then (i32.const 0)) (else (i32.mul (local.get $old) (i32.const 65536)))))
  (func (export "pc_filter") (param $buf i32) (param $len i32) (param $w i32) (param $h i32)
    (param $ch i32) (param $fmt i32) (param $pp i32) (param $pl i32) (result i32)
    (local $y i32) (local $x i32) (local $row i32) (local $stride i32) (local $px i32)
    (local.set $px (i32.mul (local.get $ch) (i32.const 4)))
    (local.set $stride (i32.mul (local.get $w) (local.get $px)))
    (block $ydone (loop $yl
      (br_if $ydone (i32.ge_u (local.get $y) (local.get $h)))
      (local.set $row (i32.add (local.get $buf) (i32.mul (local.get $y) (local.get $stride))))
      (local.set $x (i32.sub (local.get $w) (i32.const 1)))
      (block $xdone (loop $xl
        (br_if $xdone (i32.le_s (local.get $x) (i32.const 0)))
        (memory.copy (i32.add (local.get $row) (i32.mul (local.get $x) (local.get $px)))
                     (i32.add (local.get $row) (i32.mul (i32.sub (local.get $x) (i32.const 1)) (local.get $px)))
                     (local.get $px))
        (local.set $x (i32.sub (local.get $x) (i32.const 1)))
        (br $xl)))
      (local.set $y (i32.add (local.get $y) (i32.const 1)))
      (br $yl)))
    (i32.const 0))
)
