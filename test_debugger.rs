use vx_vm::{VM, DebugAction};

fn main() {
    let mut vm = VM::new();
    
    // Set a breakpoint at PC 8 (the out call)
    vm.set_breakpoint(8);
    
    // Load the compiled bytecode
    let bytecode = std::fs::read("test_debug.vxobj").expect("Failed to read bytecode");
    vm.load_module(&bytecode).expect("Failed to load module");
    
    println!("Starting VM with breakpoint at PC 8...");
    println!("Debug commands: continue/c, step/s, next/n, backtrace/bt, info locals, help/h\n");
    
    // Run the VM - this will hit the breakpoint and enter the REPL
    let result = vm.run();
    match &result {
        Ok(val) => println!("VM execution completed with result: {:?}", val),
        Err(e) => println!("VM execution error: {}", e),
    }
}