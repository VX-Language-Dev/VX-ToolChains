// VX Language LSP - Backend 结构体
// 实现 tower_lsp::LanguageServer trait，对接编辑器请求

use std::sync::Arc;
use tower_lsp::jsonrpc::Result;
use tower_lsp::lsp_types::{
    CompletionOptions, CompletionParams, CompletionResponse, DidChangeTextDocumentParams,
    DidCloseTextDocumentParams, DidOpenTextDocumentParams, DocumentSymbolParams,
    DocumentSymbolResponse, GotoDefinitionParams, GotoDefinitionResponse, Hover, HoverParams,
    InitializeParams, InitializeResult, InitializedParams, MessageType, SaveOptions,
    ServerCapabilities, ServerInfo, SymbolInformation, TextDocumentSyncCapability,
    TextDocumentSyncKind, TextDocumentSyncOptions, TextDocumentSyncSaveOptions,
    WorkspaceSymbolParams,
};
use tower_lsp::{Client, LanguageServer};

use crate::completion;
use crate::diagnostics;
use crate::goto;
use crate::hover;
use crate::state::{BackendState, DocumentState};
use crate::symbols;

pub struct VxLspBackend {
    pub client: Client,
    pub state: Arc<BackendState>,
}

impl VxLspBackend {
    pub fn new(client: Client) -> Self {
        Self {
            client,
            state: Arc::new(BackendState::new()),
        }
    }

    async fn analyze_and_publish(&self, uri: tower_lsp::lsp_types::Url, source: String) {
        let result = diagnostics::run_diagnostics(&uri, &source);

        let doc_state = DocumentState {
            source,
            tokens: result.tokens,
            ast: result.ast,
        };
        self.state.documents.insert(uri.clone(), doc_state);

        self.client
            .publish_diagnostics(uri, result.diagnostics, None)
            .await;
    }
}

#[tower_lsp::async_trait]
impl LanguageServer for VxLspBackend {
    async fn initialize(&self, _params: InitializeParams) -> Result<InitializeResult> {
        Ok(InitializeResult {
            capabilities: ServerCapabilities {
                text_document_sync: Some(TextDocumentSyncCapability::Options(
                    TextDocumentSyncOptions {
                        open_close: Some(true),
                        change: Some(TextDocumentSyncKind::FULL),
                        will_save: Some(false),
                        save: Some(TextDocumentSyncSaveOptions::SaveOptions(SaveOptions {
                            include_text: Some(true),
                        })),
                        ..Default::default()
                    },
                )),
                completion_provider: Some(CompletionOptions {
                    trigger_characters: Some(vec![".".to_string()]),
                    resolve_provider: Some(false),
                    ..Default::default()
                }),
                definition_provider: Some(tower_lsp::lsp_types::OneOf::Left(true)),
                document_symbol_provider: Some(tower_lsp::lsp_types::OneOf::Left(true)),
                workspace_symbol_provider: Some(tower_lsp::lsp_types::OneOf::Left(true)),
                ..Default::default()
            },
            server_info: Some(ServerInfo {
                name: "vx-lsp".to_string(),
                version: Some(env!("CARGO_PKG_VERSION").to_string()),
            }),
        })
    }

    async fn initialized(&self, _params: InitializedParams) {
        self.client
            .log_message(MessageType::INFO, "VX LSP 服务器已启动")
            .await;
    }

    async fn shutdown(&self) -> Result<()> {
        Ok(())
    }

    async fn did_open(&self, params: DidOpenTextDocumentParams) {
        let uri = params.text_document.uri;
        let source = params.text_document.text;
        self.analyze_and_publish(uri, source).await;
    }

    async fn did_change(&self, params: DidChangeTextDocumentParams) {
        let uri = params.text_document.uri;
        if let Some(change) = params.content_changes.into_iter().last() {
            let new_source = change.text;
            self.analyze_and_publish(uri, new_source).await;
        }
    }

    async fn did_close(&self, params: DidCloseTextDocumentParams) {
        self.state.documents.remove(&params.text_document.uri);
    }

    async fn completion(&self, params: CompletionParams) -> Result<Option<CompletionResponse>> {
        let uri = params.text_document_position.text_document.uri;
        let position = params.text_document_position.position;

        let trigger_kind = params
            .context
            .as_ref()
            .map(|c| c.trigger_kind)
            .unwrap_or(tower_lsp::lsp_types::CompletionTriggerKind::INVOKED);
        let trigger_char = params
            .context
            .and_then(|c| c.trigger_character);

        let doc = self.state.documents.get(&uri);
        let result = match doc {
            Some(d) => completion::complete(
                &d.ast,
                &d.tokens,
                &d.source,
                position,
                trigger_kind,
                trigger_char.as_deref(),
            ),
            None => None,
        };
        Ok(result)
    }

    async fn hover(&self, params: HoverParams) -> Result<Option<Hover>> {
        let uri = params.text_document_position_params.text_document.uri;
        let position = params.text_document_position_params.position;

        let doc = self.state.documents.get(&uri);
        let result = match doc {
            Some(d) => hover::hover(&d.ast, &d.tokens, &d.source, position),
            None => None,
        };
        Ok(result)
    }

    async fn goto_definition(
        &self,
        params: GotoDefinitionParams,
    ) -> Result<Option<GotoDefinitionResponse>> {
        let uri = params.text_document_position_params.text_document.uri;
        let position = params.text_document_position_params.position;

        let doc = self.state.documents.get(&uri);
        let result = match doc {
            Some(d) => goto::goto_definition(&d.ast, &d.source, position, &uri),
            None => None,
        };
        Ok(result)
    }

    async fn document_symbol(
        &self,
        params: DocumentSymbolParams,
    ) -> Result<Option<DocumentSymbolResponse>> {
        let uri = params.text_document.uri;
        let doc = self.state.documents.get(&uri);
        let result = match doc {
            Some(d) => {
                let syms = symbols::document_symbols(&d.ast);
                if syms.is_empty() {
                    None
                } else {
                    Some(DocumentSymbolResponse::Nested(syms))
                }
            }
            None => None,
        };
        Ok(result)
    }

    async fn symbol(
        &self,
        params: WorkspaceSymbolParams,
    ) -> Result<Option<Vec<SymbolInformation>>> {
        let result = symbols::workspace_symbols(&self.state.documents, &params);
        if result.is_empty() {
            Ok(None)
        } else {
            Ok(Some(result))
        }
    }
}