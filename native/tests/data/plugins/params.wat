;; A Genko test module of PhotoCraft's plug-in ABI v1 (PC5 J-f, docs/PLUGIN_WASM.md): its manifest at 16, a bump
;; allocator on memory.grow (each band gets a fresh instance), and pc_filter as below. Made into .wasm once with
;; `cargo run --example wat2wasm` of native/wasm-host (native/tests/data/plugins/MANIFEST.json).
(module
  
  (memory (export "memory") 1)
  (data (i32.const 16) "{\22id\22:\22test.params\22,\22name\22:\22Params\22,\22version\22:\221\22,\22kind\22:\22filter\22,\22params\22:{\22amount\22:{\22type\22:\22number\22,\22min\22:0,\22max\22:10,\22default\22:5},\22mono\22:{\22type\22:\22bool\22,\22default\22:false},\22mode\22:{\22type\22:\22choice\22,\22options\22:[\22soft\22,\22hard\22],\22default\22:\22soft\22}}}")
  (func (export "pc_abi_version") (result i32) (i32.const 1))
  (func (export "pc_manifest") (result i64)
    (i64.or (i64.shl (i64.const 240) (i64.const 32)) (i64.const 16)))
  (func (export "pc_alloc") (param $n i32) (result i32) (local $old i32)
    (local.set $old (memory.grow (i32.shr_u (i32.add (local.get $n) (i32.const 65535)) (i32.const 16))))
    (if (result i32) (i32.eq (local.get $old) (i32.const -1))
      (then (i32.const 0)) (else (i32.mul (local.get $old) (i32.const 65536)))))
  (func (export "pc_filter") (param $buf i32) (param $len i32) (param $w i32) (param $h i32)
    (param $ch i32) (param $fmt i32) (param $pp i32) (param $pl i32) (result i32)
    (local $i i32) (local $c i32) (local $alpha i32)
    (local.set $alpha (i32.ne (i32.and (local.get $fmt) (i32.const 256)) (i32.const 0)))
    (block $done (loop $l
      (br_if $done (i32.ge_u (local.get $i) (local.get $len)))
      (local.set $c (i32.rem_u (i32.shr_u (local.get $i) (i32.const 2)) (local.get $ch)))
      (if (i32.eqz (i32.and (local.get $alpha) (i32.eq (local.get $c) (i32.sub (local.get $ch) (i32.const 1)))))
        (then (f32.store (i32.add (local.get $buf) (local.get $i)) (f32.div (f32.convert_i32_u (local.get $pl)) (f32.const 1000)))))
      (local.set $i (i32.add (local.get $i) (i32.const 4)))
      (br $l)))
    (i32.const 0))
)
