// VX Language LSP - 主入口
// 启动 tokio 异步运行时，加载 tower-lsp 服务，通过 stdin/stdout 与编辑器通信

use tower_lsp::{LspService, Server};

mod backend;
mod completion;
mod diagnostics;
mod goto;
mod hover;
mod state;
mod symbols;

use backend::VxLspBackend;

#[tokio::main]
async fn main() {
    let stdin = tokio::io::stdin();
    let stdout = tokio::io::stdout();

    let (service, socket) = LspService::new(VxLspBackend::new);
    Server::new(stdin, stdout, socket).serve(service).await;
}
