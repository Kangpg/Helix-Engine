using Helix.Engine;

await using var server = HelixServer.Create(args.Length > 0 ? args[0] : "config/helix.toml");
Console.CancelKeyPress += (_, e) => { e.Cancel = true; server.Stop(); };
await server.RunAsync(new DelegateServerApp((session, message) =>
{
	if (!session.Send(message)) session.Close(CloseReason.IoError);
}));
