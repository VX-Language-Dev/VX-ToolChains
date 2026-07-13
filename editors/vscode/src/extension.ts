import * as path from 'path';
import * as fs from 'fs';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';

const LANGUAGE_ID = 'vx';
const LSP_BINARY_NAME = 'vx-lsp';
const OUTPUT_CHANNEL_NAME = 'VX Language';

/**
 * 向 VS Code 注册 VX 语言服务器的 LSP 能力。
 * 当前激活的能力包括：
 * - 文本同步（全量）
 * - 自动补全（触发字符：. 和 ->）
 * - 悬停提示
 * - 跳转到定义
 * - 文档符号 / 工作区符号
 * - 诊断信息
 */

let client: LanguageClient | undefined;

export function activate(context: vscode.ExtensionContext): void {
    const outputChannel = vscode.window.createOutputChannel(OUTPUT_CHANNEL_NAME);

    const config = vscode.workspace.getConfiguration('vx.languageServer');
    const configuredPath = config.get<string>('path', '').trim();
    const trace = config.get<boolean>('trace', false);

    const serverPath = resolveServerPath(configuredPath, outputChannel);

    if (!serverPath) {
        outputChannel.appendLine(
            `[VX] LSP server binary not found. Falling back to syntax highlighting only.`
        );
        outputChannel.appendLine(
            `[VX] Set 'vx.languageServer.path' or build with: cargo build --bin vx-lsp --release`
        );
        outputChannel.show(true);
        return;
    }

    outputChannel.appendLine(`[VX] Using LSP server: ${serverPath}`);

    const serverOptions: ServerOptions = {
        run: {
            command: serverPath,
            transport: TransportKind.stdio,
        },
        debug: {
            command: serverPath,
            transport: TransportKind.stdio,
        },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: 'file', language: LANGUAGE_ID },
            { scheme: 'untitled', language: LANGUAGE_ID },
        ],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.vx'),
        },
        outputChannel,
        traceOutputChannel: trace ? outputChannel : undefined,
    };

    client = new LanguageClient(
        'vxLanguageServer',
        'VX Language Server',
        serverOptions,
        clientOptions
    );

    client.start().catch((err) => {
        outputChannel.appendLine(`[VX] Failed to start LSP server: ${err}`);
        vscode.window.showWarningMessage(
            `VX LSP server could not be started. Syntax highlighting is still available.`
        );
    });

    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (e.affectsConfiguration('vx.languageServer')) {
                vscode.window.showInformationMessage(
                    'VX: Restart VS Code to apply language server configuration changes.'
                );
            }
        })
    );
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}

function resolveServerPath(
    configuredPath: string,
    outputChannel: vscode.OutputChannel
): string | undefined {
    if (configuredPath) {
        if (fs.existsSync(configuredPath)) {
            return configuredPath;
        }
        outputChannel.appendLine(
            `[VX] Configured server path does not exist: ${configuredPath}`
        );
    }

    const vlsEnvPath = process.env.VLS;
    if (vlsEnvPath) {
        if (fs.existsSync(vlsEnvPath)) {
            outputChannel.appendLine(`[VX] Using VLS environment variable: ${vlsEnvPath}`);
            return vlsEnvPath;
        }
        outputChannel.appendLine(
            `[VX] VLS environment variable set but path does not exist: ${vlsEnvPath}`
        );
    }

    const workspaceFolders = vscode.workspace.workspaceFolders;
    if (!workspaceFolders || workspaceFolders.length === 0) {
        return undefined;
    }

    const candidates: string[] = [];
    for (const folder of workspaceFolders) {
        let root = folder.uri.fsPath;
        // If the workspace is the extension folder itself, go up to project root
        if (root.endsWith('editors/vscode') || root.endsWith('editors\\vscode')) {
            root = path.dirname(path.dirname(root));
        }
        candidates.push(
            path.join(root, 'target', 'release', LSP_BINARY_NAME),
            path.join(root, 'target', 'debug', LSP_BINARY_NAME)
        );
    }

    for (const candidate of candidates) {
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }

    return undefined;
}