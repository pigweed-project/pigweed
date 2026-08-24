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
use prost::Message;
use pw_gdb_protocol::{Client, StopReply};
use pw_kernel_annotations::ImageInfo;
use pw_kernel_debug_mailbox_client::DebugMailboxClient;
use pw_kernel_debug_mailbox_protocol::HostCommand;
use runfiles::{Runfiles, rlocation};
use tokio::fs;
use tokio::net::TcpStream;
use tokio::process::{Child, Command};
use tokio_util::compat::{Compat, TokioAsyncReadCompatExt};
use trace_proto::perfetto::protos::Trace;

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

fn start_qemu(
    qemu_cpu: &str,
    qemu_machine: &str,
    qemu_runner_path: &Path,
    image_path: &Path,
) -> (Child, u16) {
    let gdb_port = find_free_port();
    println!("Starting QEMU with GDB server on port: {}", gdb_port);

    let mut child = Command::new(qemu_runner_path)
        .args([
            "--cpu",
            qemu_cpu,
            "--machine",
            qemu_machine,
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
    mailbox: &DebugMailboxClient,
) {
    // Continue target execution
    gdb_client.continue_execution().await.unwrap();

    mailbox.wait_until_ready(gdb_client).await.unwrap();
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

async fn check_trace(trace_path: &Path) {
    assert!(trace_path.exists(), "Trace output file was not generated");

    let trace_bytes = fs::read(trace_path)
        .await
        .unwrap_or_else(|err| panic!("Failed to read {}: {}", trace_path.display(), err));
    let trace = Trace::decode(trace_bytes.as_slice())
        .unwrap_or_else(|err| panic!("Failed to decode {}: {}", trace_path.display(), err));

    assert!(!trace.packet.is_empty(), "Trace contains no packets");

    let has_threads = trace.packet.iter().any(|p| {
        matches!(
            p.data,
            Some(trace_proto::perfetto::protos::trace_packet::Data::TrackDescriptor(ref td))
                if td.thread.is_some()
        )
    });
    assert!(has_threads, "Trace missing thread track descriptors");
    println!(
        "Successfully verified Perfetto trace with {} packets and thread tracks!",
        trace.packet.len()
    );
}

fn resolve_path(r: &Runfiles, path: &Path, name: &str) -> PathBuf {
    let resolved = rlocation!(r, path)
        .unwrap_or_else(|| panic!("Could not find {} in runfiles ({})", name, path.display()));
    assert!(resolved.exists(), "{} does not exist", name);
    resolved
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn core::error::Error>> {
    let args = Args::parse();

    let r = Runfiles::create().expect("Failed to initialize Bazel runfiles");

    let qemu_runner_path =
        resolve_path(&r, Path::new("_main/pw_kernel/tooling/qemu"), "qemu runner");
    let k_tool_path = resolve_path(&r, Path::new("_main/pw_kernel/tooling/k/k"), "k tool");
    let image_path = resolve_path(&r, &args.image, "system image");

    let image_info = ImageInfo::new(&image_path).expect("could not parse image info");

    println!("{:?}", image_info.mailboxes);
    let mailbox = DebugMailboxClient::lookup(&image_info, "test_mailbox").unwrap();

    let (mut qemu, gdb_port) = start_qemu(&args.cpu, &args.machine, &qemu_runner_path, &image_path);

    let gdb_sock = connect_gdb_with_retry(gdb_port, &mut qemu).await;
    let compat_stream = gdb_sock.compat();
    let mut gdb_client = Client::new(compat_stream);
    if image_info.architecture == object::Architecture::Arm {
        gdb_client.set_has_arm_trustzone(true);
    }

    run_until_exit_and_hang(&mut gdb_client, &mailbox).await;

    if let Err(e) = gdb_client.set_qemu_physical_memory_mode(true).await {
        println!("Failed to set QEMU physical memory mode: {:?}", e);
    } else {
        println!("Successfully enabled QEMU physical memory mode!");
    }

    // Close the active GDB socket temporarily so tooling/k can connect
    drop(gdb_client);

    let trace_path = grab_trace(&image_path, &k_tool_path, gdb_port).await;

    fs::copy(trace_path, args.output_file.clone())
        .await
        .expect("failed to copy trace.pb into test outputs dir");
    println!("Copied trace.pb to {}", args.output_file.display());

    println!("Attempting to reconnect to qemu gdb to allow for clean exit.");
    let gdb_sock = connect_gdb_with_retry(gdb_port, &mut qemu).await;
    let compat_stream = gdb_sock.compat();
    let mut gdb_client = Client::new(compat_stream);
    if image_info.architecture == object::Architecture::Arm {
        gdb_client.set_has_arm_trustzone(true);
        gdb_client
            .set_qemu_physical_memory_mode(true)
            .await
            .unwrap();
    }

    {
        gdb_client.continue_execution().await.unwrap();
        mailbox
            .send(&mut gdb_client, HostCommand::Exit)
            .await
            .unwrap();
        let stop_reply = gdb_client.wait_for_stop_reply().await.unwrap();
        println!("GDB target stopped: {:?}", stop_reply);

        assert!(
            matches!(stop_reply, StopReply::Exited(0)),
            "Unexpected stop reply: {:?}",
            stop_reply
        );
    }

    check_trace(&args.output_file).await;

    Ok(())
}
