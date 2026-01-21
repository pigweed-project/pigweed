// Copyright 2025 The Pigweed Authors
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

use anyhow::Result;
mod image_info;
mod stacks;

use std::path::{Path, PathBuf};

use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(name = "k")]
#[command(about = "Pigweed Kernel CLI Tool", long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Print information about a kernel image
    #[command(name = "image_info")]
    ImageInfo {
        #[arg(required = true)]
        path: PathBuf,
    },
    /// Print stack usage information from a running target
    #[command(name = "stacks")]
    Stacks {
        #[arg(required = true)]
        path: PathBuf,
        #[arg(long, default_value = "localhost:1234")]
        gdb: String,
    },
}

fn print_image_info(path: &Path) -> Result<()> {
    let info = image_info::ImageInfo::new(path)?;
    println!("Stacks:");
    println!("  {:<30} {:<12} {:<10}", "Name", "Address", "Size");
    println!("  {:-<30} {:-<12} {:-<10}", "", "", "");
    for stack in info.stacks {
        println!(
            "  {:<30} 0x{:08x}   0x{:x}",
            stack.name, stack.stack_addr, stack.stack_size
        );
    }

    println!();
    println!("Threads:");
    println!("  {:<30} {:<12} {:<12}", "Name", "Id", "Parent");
    println!("  {:-<30} {:-<12} {:-<12}", "", "", "");
    for thread in info.threads {
        if thread.parent_id == 0x0 {
            println!("  {:<30} 0x{:08x}   kernel", thread.name, thread.id);
        } else {
            println!(
                "  {:<30} 0x{:08x}   0x{:08x}",
                thread.name, thread.id, thread.parent_id
            );
        }
    }

    println!();
    println!("Processes:");
    println!("  {:<30} {:<12}", "Name", "Id");
    println!("  {:-<30} {:-<12}", "", "");
    for process in info.processes {
        println!("  {:<30} 0x{:08x}", process.name, process.id);
    }

    Ok(())
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();

    match &cli.command {
        Commands::ImageInfo { path } => {
            print_image_info(path)?;
        }
        Commands::Stacks { path, gdb } => {
            stacks::run(path, gdb).await?;
        }
    }

    Ok(())
}
