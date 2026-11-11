//! Erro único exposto à UI: código estável (a UI decide o texto/ação por ele) + mensagem pt-BR.

use serde::Serialize;
use std::fmt;
use std::path::Path;

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct AppError {
    pub code: &'static str,
    pub message: String,
}

pub type AppResult<T> = Result<T, AppError>;

impl AppError {
    pub fn new(code: &'static str, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }

    /// Configuração recusada pela validação.
    pub fn invalid_settings(message: impl Into<String>) -> Self {
        Self::new("invalid_settings", message)
    }

    /// Falha de disco; a mensagem nomeia a pasta/arquivo para o usuário saber onde olhar.
    pub fn io(path: &Path, err: &std::io::Error) -> Self {
        let code = match err.kind() {
            std::io::ErrorKind::PermissionDenied => "permission_denied",
            std::io::ErrorKind::StorageFull => "disk_full",
            _ => "io_error",
        };
        Self::new(
            code,
            format!("Não foi possível gravar em {}: {err}", path.display()),
        )
    }
}

impl fmt::Display for AppError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.code, self.message)
    }
}

impl std::error::Error for AppError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serializa_codigo_e_mensagem() {
        let e = AppError::invalid_settings("Endereço do Hub inválido");
        let v = serde_json::to_value(&e).unwrap();
        assert_eq!(
            v,
            serde_json::json!({ "code": "invalid_settings", "message": "Endereço do Hub inválido" })
        );
    }

    #[test]
    fn io_mapeia_permissao_e_disco_cheio() {
        let p = Path::new("C:/x");
        let denied = std::io::Error::from(std::io::ErrorKind::PermissionDenied);
        assert_eq!(AppError::io(p, &denied).code, "permission_denied");
        let full = std::io::Error::from(std::io::ErrorKind::StorageFull);
        assert_eq!(AppError::io(p, &full).code, "disk_full");
        let other = std::io::Error::other("x");
        let e = AppError::io(p, &other);
        assert_eq!(e.code, "io_error");
        assert!(e.message.contains("C:/x"));
    }
}
