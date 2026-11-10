-- Eco de exemplo (usado pelos bots do teste de carga): o evento de cliente 'echo' volta só para quem mandou,
-- com o mesmo payload JSON.
cb.events.on('echo', function(p, data)
  cb.events.emit(p.id, 'echo', data)
end)
