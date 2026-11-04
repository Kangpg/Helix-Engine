namespace Helix.Engine.Transport;

internal static class TransportFactory
{
	public static IServerTransport Create(EngineConfig config) => config.Transport switch
	{
		"tcp" => new TcpServerTransport(config),
		_ => throw new NotSupportedException("Selected transport is not implemented.")
	};
}
