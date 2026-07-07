// Copyright 2026 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

use core::time::Duration;
use std::path::{Path, PathBuf};
use std::process::Stdio;

use clap::Parser;
use object::{File, Object, ObjectSymbol};
use pw_gdb_protocol::{Client, StopReply};
use runfiles::{Runfiles, rlocation};
use tokio::fs;
use tokio::net::TcpStream;
use tokio::process::{Child, Command};
use tokio_util::compat::{Compat, TokioAsyncReadCompatExt};

#[derive(Parser, Debug)]
#[command(name = "tracing_test")]
struct Args {
    #[arg(long)]
    cpu: String,

    #[arg(long)]
    machine: String,

    #[arg(long)]
    image: PathBuf,

    #[arg(long)]
    output_file: PathBuf,
}

fn find_free_port() -> u16 {
    std::net::TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}

async fn connect_gdb_with_retry(port: u16, child: &mut Child) -> tokio::net::TcpStream {
    let addr = format!("127.0.0.1:{}", port);
    let mut retries = 50;
    while retries > 0 {
        if let Ok(Some(status)) = child.try_wait() {
            panic!("QEMU process exited early with status: {}", status);
        }

        println!(
            "Connection attempt {}/50 to {} (gdb)...",
            51 - retries,
            addr
        );

        match TcpStream::connect(&addr).await {
            Ok(stream) => {
                return stream;
            }
            Err(_) => {
                tokio::time::sleep(Duration::from_millis(100)).await;
                retries -= 1;
            }
        }
    }
    panic!("Failed to connect to QEMU gdb socket on port {}", port);
}

fn start_qemu(cli_args: &Args, qemu_runner_path: &Path, image_path: &Path) -> (Child, u16) {
    let gdb_port = find_free_port();
    println!("Starting QEMU with GDB server on port: {}", gdb_port);

    let mut child = Command::new(qemu_runner_path)
        .args([
            "--cpu",
            &cli_args.cpu,
            "--machine",
            &cli_args.machine,
            "--semihosting",
            "--image",
            image_path.to_str().unwrap(),
            "--gdb-tcp-port",
            &gdb_port.to_string(),
            "--gdb-pause",
        ])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()
        .expect("Failed to spawn QEMU runner");

    let stdout = child.stdout.take().unwrap();
    let stderr = child.stderr.take().unwrap();

    tokio::spawn(async move {
        use tokio::io::{AsyncBufReadExt, BufReader};
        let mut lines = BufReader::new(stdout).lines();
        while let Ok(Some(line)) = lines.next_line().await {
            println!("[QEMU STDOUT] {}", line);
        }
    });

    tokio::spawn(async move {
        use tokio::io::{AsyncBufReadExt, BufReader};
        let mut lines = BufReader::new(stderr).lines();
        while let Ok(Some(line)) = lines.next_line().await {
            println!("[QEMU STDERR] {}", line);
        }
    });

    (child, gdb_port)
}

async fn run_until_exit_and_hang(
    gdb_client: &mut Client<Compat<tokio::net::TcpStream>>,
    image_path: &Path,
) -> (u64, u64) {
    let image_data = fs::read(&image_path)
        .await
        .expect("Failed to read ELF file");
    let obj_file = File::parse(&*image_data).expect("Failed to parse ELF file");

    let (shutdown_addr, instruction_width) = {
        // Since we're acting as the gdb client, we are expected
        // to lookup the symbol address ourselves
        let symbol = obj_file
            .symbol_by_name("pw_kernel_target_shutdown")
            .expect("could not find symbol");
        let mut addr = symbol.address();

        // The LSB is set to indicate an ARM thumb instruction,
        // but we just want the physical address, which is 16-bit aligned
        if obj_file.architecture() == object::Architecture::Arm {
            addr &= !1;
        };

        let instruction_width = match obj_file.architecture() {
            object::Architecture::Arm => 2, // Thumb
            _ => 4,                         // assuming our target is 32-bit
        };

        (addr, instruction_width)
    };

    println!(
        "Found pw_kernel_target_shutdown address: {:#010x}",
        shutdown_addr
    );

    gdb_client
        .insert_software_breakpoint(shutdown_addr, instruction_width)
        .await
        .unwrap();
    println!(
        "GDB set breakpoint at {:#010x} (width={})",
        shutdown_addr, instruction_width
    );

    // Continue target execution
    let stop_reply = gdb_client.continue_execution().await.unwrap();
    println!("GDB target stopped: {:?}", stop_reply);

    assert!(
        matches!(stop_reply, StopReply::Signal(5)),
        "Unexpected stop reply: {:?}",
        stop_reply
    );

    (shutdown_addr, instruction_width)
}

async fn grab_trace(image_path: &Path, k_tool_path: &Path, gdb_port: u16) -> PathBuf {
    // Run tooling/k to grab the trace!
    let mut k_cmd = Command::new(k_tool_path);
    k_cmd.args([
        "trace",
        image_path.to_str().unwrap(),
        "--gdb",
        &format!("localhost:{}", gdb_port),
    ]);

    println!("Invoking k tool: {:?}", k_cmd);
    let k_output = k_cmd.output().await.expect("Failed to run k tool");
    println!(
        "k tool stdout:\n{}",
        String::from_utf8_lossy(&k_output.stdout)
    );
    println!(
        "k tool stderr:\n{}",
        String::from_utf8_lossy(&k_output.stderr)
    );
    assert!(k_output.status.success(), "k tool failed");

    // Verify that trace.pb was successfully written and has size > 0
    let trace_path = Path::new("trace.pb");
    assert!(trace_path.exists(), "Trace file trace.pb was not created");

    trace_path.to_path_buf()
}

fn check_trace(trace_path: &Path) {
    let metadata = trace_path.metadata().unwrap();
    assert!(metadata.len() > 0, "Trace file is empty");
    println!(
        "Successfully generated Perfetto trace of size {} bytes!",
        metadata.len()
    );
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn core::error::Error>> {
    let args = Args::parse();

    let r = Runfiles::create().expect("Failed to initialize Bazel runfiles");

    let qemu_runner_path = rlocation!(r, "_main/pw_kernel/tooling/qemu")
        .expect("Could not find qemu runner in runfiles");
    assert!(qemu_runner_path.exists(), "qemu runner does not exist");

    let k_tool_path =
        rlocation!(r, "_main/pw_kernel/tooling/k/k").expect("Could not find k tool in runfiles");
    assert!(k_tool_path.exists(), "k tool does not exist");

    let image_path = rlocation!(r, &args.image).unwrap_or_else(|| {
        panic!(
            "Could not find system image in runfiles ({})",
            args.image.display()
        )
    });
    assert!(image_path.exists(), "image does not exist");

    let (mut qemu, gdb_port) = start_qemu(&args, &qemu_runner_path, &image_path);

    let gdb_sock = connect_gdb_with_retry(gdb_port, &mut qemu).await;
    let compat_stream = gdb_sock.compat();
    let mut gdb_client = Client::new(compat_stream);

    let (shutdown_breakpoint_address, instruction_width) =
        run_until_exit_and_hang(&mut gdb_client, &image_path).await;

    // Close the active GDB socket temporarily so tooling/k can connect
    drop(gdb_client);

    let trace_path = grab_trace(&image_path, &k_tool_path, gdb_port).await;

    fs::copy(trace_path, args.output_file.clone())
        .await
        .expect("failed to copy trace.pb into test outputs dir");
    println!("Copied trace.pb to {}", args.output_file.display());

    check_trace(&args.output_file);

    println!("Attempting to reconnect to qemu gdb to allow for clean exit.");
    let gdb_sock = connect_gdb_with_retry(gdb_port, &mut qemu).await;
    let compat_stream = gdb_sock.compat();
    let mut gdb_client = Client::new(compat_stream);

    {
        println!("Clearing breakpoint.");
        gdb_client
            .remove_software_breakpoint(shutdown_breakpoint_address, instruction_width)
            .await
            .unwrap();

        let stop_reply = gdb_client.continue_execution().await.unwrap();
        println!("GDB target stopped: {:?}", stop_reply);

        assert!(
            matches!(stop_reply, StopReply::Exited(0)),
            "Unexpected stop reply: {:?}",
            stop_reply
        );
    }

    Ok(())
}
