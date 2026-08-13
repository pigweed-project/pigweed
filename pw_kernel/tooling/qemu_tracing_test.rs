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
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Stdio;

use clap::Parser;
use futures::io::{AsyncRead, AsyncWrite};
use prost::Message;
use prost_reflect::text_format::FormatOptions;
use prost_reflect::{DescriptorPool, DynamicMessage, MessageDescriptor};
use pw_gdb_protocol::{Client, StopReply};
use pw_kernel_annotations::{DebugMailboxInfo, ImageInfo};
use runfiles::{Runfiles, rlocation};
use tokio::fs;
use tokio::net::TcpStream;
use tokio::process::{Child, Command};
use tokio_util::compat::{Compat, TokioAsyncReadCompatExt};
use trace_proto::perfetto::protos::generic_kernel_task_state_event::TaskStateEnum;
use trace_proto::perfetto::protos::{Trace, trace_packet};

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

    #[arg(long)]
    golden_file: PathBuf,

    #[arg(long)]
    trace_proto_descriptor: PathBuf,
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

struct Mailbox(DebugMailboxInfo);

impl Mailbox {
    fn lookup_from_image(image: &ImageInfo, name: &str) -> Self {
        for mailbox in &image.mailboxes {
            if mailbox.name == name {
                return Self(mailbox.clone());
            }
        }
        panic!("Mailbox not found: {}", name);
    }

    async fn read_field<S: AsyncRead + AsyncWrite + Unpin>(
        &self,
        gdb_client: &mut Client<S>,
        field_num: u64,
    ) -> u32 {
        let read_addr = self.0.addr + field_num * 4;

        // Hack to get QEMU to allow us to read protected memory from userspace on arm
        gdb_client
            .set_qemu_physical_memory_mode(true)
            .await
            .unwrap();

        let value = gdb_client.read_memory(read_addr, 4).await.unwrap();

        gdb_client
            .set_qemu_physical_memory_mode(false)
            .await
            .unwrap();

        u32::from_le_bytes(value[..4].try_into().unwrap())
    }

    async fn write_field<S: AsyncRead + AsyncWrite + Unpin>(
        &self,
        gdb_client: &mut Client<S>,
        field_num: u64,
        value: u32,
    ) {
        let write_addr = self.0.addr + field_num * 4;

        // Hack to get QEMU to allow us to read protected memory from userspace on arm
        gdb_client
            .set_qemu_physical_memory_mode(true)
            .await
            .unwrap();

        gdb_client
            .write_memory(write_addr, &value.to_le_bytes())
            .await
            .unwrap();
    }

    pub async fn wait_until_ready<S: AsyncRead + AsyncWrite + Unpin>(
        &self,
        gdb_client: &mut Client<S>,
    ) {
        loop {
            gdb_client.interrupt().await.unwrap();

            let ready = self.read_field(gdb_client, 0).await;
            if ready != 0 {
                break;
            }

            gdb_client.continue_execution().await.unwrap();
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    }

    pub async fn send<S: AsyncRead + AsyncWrite + Unpin>(
        &self,
        gdb_client: &mut Client<S>,
        value: u32,
    ) {
        self.wait_until_ready(gdb_client).await;

        // Write the value
        self.write_field(gdb_client, 2, value).await;

        // Kick
        self.write_field(gdb_client, 1, 1).await;

        println!("Kicking mailbox {}!", self.0.name);
        gdb_client.continue_execution().await.unwrap()
    }
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
    mailbox: &Mailbox,
) {
    // Continue target execution
    gdb_client.continue_execution().await.unwrap();

    mailbox.wait_until_ready(gdb_client).await;
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

async fn load_textproto_trace(
    textproto_path: &Path,
    proto_descriptor: &MessageDescriptor,
) -> Trace {
    let text = fs::read_to_string(textproto_path)
        .await
        .unwrap_or_else(|err| {
            panic!(
                "Failed to read textproto trace file {}: {}",
                textproto_path.display(),
                err
            )
        });
    let dynamic_message = DynamicMessage::parse_text_format(proto_descriptor.clone(), &text)
        .expect("Failed to parse textproto");

    // Convert DynamicMessage to static Trace
    let mut bytes = Vec::new();
    dynamic_message
        .encode(&mut bytes)
        .expect("Failed to encode dynamic message");

    Trace::decode(bytes.as_slice()).expect("Failed to decode textproto")
}

async fn check_trace(
    trace_path: &Path,
    golden_file_path: &Path,
    trace_proto_descriptor: &MessageDescriptor,
) {
    println!("Checking trace against goldenfile");

    let trace_data = fs::read(&trace_path)
        .await
        .unwrap_or_else(|err| panic!("Failed to read {}: {}", trace_path.display(), err));
    let trace = Trace::decode(trace_data.as_slice())
        .unwrap_or_else(|err| panic!("Failed to decode {}: {}", trace_path.display(), err));
    println!("Decoded trace containing {} packets", trace.packet.len());

    // Load and parse textproto golden file
    let golden_trace = load_textproto_trace(golden_file_path, trace_proto_descriptor).await;

    let matcher = TraceMatcher::new(golden_trace, trace);

    matcher.check_tracks();
    matcher.check_thread_states();

    println!("Successfully verified scheduling switch events!");
}

async fn write_textproto(
    output_file: &Path,
    golden_file: &Path,
    trace_proto_descriptor: &MessageDescriptor,
) {
    // Decode generated trace to DynamicMessage to format as textproto
    let trace_data = fs::read(output_file)
        .await
        .expect("Failed to read copied trace.pb");
    let dynamic_message =
        DynamicMessage::decode(trace_proto_descriptor.clone(), trace_data.as_slice())
            .expect("Failed to decode generated trace.pb into DynamicMessage");
    let format_options = FormatOptions::new().pretty(true);
    let textproto = dynamic_message.to_text_format_with_options(&format_options);

    // Write the actual textproto to the output directory (same directory as output_file)
    let golden_filename = golden_file
        .file_name()
        .expect("golden_file missing filename");
    let output_textproto_path = output_file
        .parent()
        .map(|p| p.join(golden_filename))
        .unwrap_or_else(|| PathBuf::from(golden_filename));

    fs::write(&output_textproto_path, textproto)
        .await
        .expect("Failed to write actual textproto to outputs directory");
    println!(
        "Wrote actual textproto to {}",
        output_textproto_path.display()
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
    let golden_file_path = resolve_path(&r, &args.golden_file, "golden file");
    let trace_proto_descriptor_path =
        resolve_path(&r, &args.trace_proto_descriptor, "trace proto descriptor");

    let image_info = ImageInfo::new(&image_path).expect("could not parse image info");

    println!("{:?}", image_info.mailboxes);
    let mailbox = Mailbox::lookup_from_image(&image_info, "test_mailbox");

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
        mailbox.send(&mut gdb_client, 0xdecafbad).await;
        let stop_reply = gdb_client.wait_for_stop_reply().await.unwrap();
        println!("GDB target stopped: {:?}", stop_reply);

        assert!(
            matches!(stop_reply, StopReply::Exited(0)),
            "Unexpected stop reply: {:?}",
            stop_reply
        );
    }

    // Load proto descriptor pool for textproto parsing
    let trace_proto_descriptor = {
        let descriptor_bytes = fs::read(trace_proto_descriptor_path)
            .await
            .expect("Failed to read descriptor set");
        let pool = DescriptorPool::decode(descriptor_bytes.as_slice())
            .expect("Failed to parse descriptor set");
        pool.get_message_by_name("perfetto.protos.Trace")
            .expect("Failed to find Trace message in descriptor pool")
    };

    write_textproto(
        &args.output_file,
        &args.golden_file,
        &trace_proto_descriptor,
    )
    .await;

    check_trace(
        &args.output_file,
        &golden_file_path,
        &trace_proto_descriptor,
    )
    .await;

    Ok(())
}

struct TraceMatcher {
    golden: Trace,
    test: Trace,

    golden_threads: HashMap<i64, String>,
    test_threads: HashMap<i64, String>,
}

impl TraceMatcher {
    pub fn new(golden: Trace, test: Trace) -> Self {
        let golden_threads = Self::get_thread_names(&golden).collect::<HashMap<i64, String>>();
        let test_threads = Self::get_thread_names(&test).collect::<HashMap<i64, String>>();

        Self {
            golden,
            test,
            golden_threads,
            test_threads,
        }
    }

    fn get_thread_names(trace: &Trace) -> impl Iterator<Item = (i64, String)> {
        trace
            .packet
            .iter()
            .filter_map(|packet| match packet.data {
                Some(trace_packet::Data::TrackDescriptor(ref td)) => td.thread.as_ref(),
                _ => None,
            })
            .map(|thread| {
                let name = thread
                    .thread_name
                    .as_ref()
                    .expect("thread track missing name");
                let tid = thread.tid.expect("thread track missing tid");

                (tid, name.clone())
            })
    }

    /// Scan through the trace for task state transitions associated with a given thread.
    ///
    /// These states come from the scheduler, so they document things like the task being running, idle, or ready to be run.
    fn get_transitions_for_thread(trace: &Trace, tid: i64) -> impl Iterator<Item = TaskStateEnum> {
        trace
            .packet
            .iter()
            .filter_map(|packet| match packet.data {
                Some(trace_packet::Data::GenericKernelTaskStateEvent(ref se)) => Some(se),
                _ => None,
            })
            .filter(move |se| se.tid.expect("state event missing thread id") == tid)
            .map(|se| {
                TaskStateEnum::try_from(se.state.expect("state event missing state"))
                    .expect("state event contins invalid state")
            })
    }

    /// Check that all of the perfetto tracks match in important metadata
    ///
    /// This means that the same threads exist, and currently no other kinds of tracks are added (like counters or other types of events).
    pub fn check_tracks(&self) {
        assert_eq!(
            self.test_threads, self.golden_threads,
            "map of threads to thread ids should match"
        );
    }

    /// Check that the sequence of states within each thread matches the golden trace.
    pub fn check_thread_states(&self) {
        // Check each thread separately because we're only verifying that tracing works.
        // We don't care about the order of things from the scheduler.
        for (tid, thread_name) in self.test_threads.iter() {
            let golden_seq = Self::get_transitions_for_thread(&self.golden, *tid)
                .collect::<Vec<TaskStateEnum>>();
            let test_seq =
                Self::get_transitions_for_thread(&self.test, *tid).collect::<Vec<TaskStateEnum>>();

            assert_eq!(
                golden_seq, test_seq,
                "sequence of states differs for thread {} with tid {}",
                thread_name, tid
            );
        }
    }
}
