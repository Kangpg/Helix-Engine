using System.Globalization;
using System.Net;

namespace Helix.Engine;

public sealed record EngineConfig
{
	public string Ip { get; init; } = "0.0.0.0";
	public int Port { get; init; } = 5000;
	public string Transport { get; init; } = "tcp";
	public int MaxConnections { get; init; } = 10000;
	public int Backlog { get; init; } = 128;
	public int MaxSendQueue { get; init; } = 1024;
	public int HeaderSize { get; init; } = 4;
	public bool BigEndian { get; init; } = true;
	public int MaxMessageSize { get; init; } = 1048576;
	public int DrainTimeoutMs { get; init; } = 5000;
	public bool ClusterEnabled { get; init; }
	public int WorldMaxEntities { get; init; } = 10000;
	public int WorldMaxPendingCommands { get; init; } = 4096;
	public int WorldMaxCommandsPerTick { get; init; } = 1024;
	public int WorldTickMs { get; init; } = 50;
	public string ProtocolSerializer { get; init; } = "protobuf";

	public void Validate()
	{
		if (!IPAddress.TryParse(Ip, out _) || Port is < 0 or > 65535)
			throw new ArgumentException("Invalid IP or port (0 is allowed for an ephemeral programmatic listener).");
		if (string.IsNullOrWhiteSpace(Transport)) throw new ArgumentException("Transport name is required.");
		if (string.IsNullOrWhiteSpace(ProtocolSerializer)) throw new ArgumentException("Serializer name is required.");
		if (MaxConnections <= 0 || Backlog <= 0 || MaxSendQueue <= 0 || DrainTimeoutMs <= 0 ||
			WorldMaxEntities <= 0 || WorldMaxPendingCommands <= 0 || WorldMaxCommandsPerTick <= 0 || WorldTickMs <= 0)
			throw new ArgumentException("Connection, queue, shutdown and MMO limits must be positive.");
		if (HeaderSize is not (2 or 4) || MaxMessageSize <= 0 || MaxMessageSize > int.MaxValue - HeaderSize ||
			(HeaderSize == 2 && MaxMessageSize > ushort.MaxValue))
			throw new ArgumentException("Invalid frame header or maximum message size.");
	}

	public static EngineConfig Load(string path)
	{
		var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
		var section = "";
		if (File.Exists(path))
		{
			foreach (var raw in File.ReadLines(path))
			{
				var line = StripComment(raw).Trim();
				if (line.Length == 0) continue;
				if (line.StartsWith('[') && line.EndsWith(']')) { section = line[1..^1].Trim(); continue; }
				var split = line.IndexOf('=');
				if (split <= 0) throw new FormatException($"Invalid configuration line: {raw}");
				values[$"{section}.{line[..split].Trim()}"] = line[(split + 1)..].Trim().Trim('"', '\'');
			}
		}
		string Value(string key, string fallback)
		{
			var parts = key.Split('.');
			return Environment.GetEnvironmentVariable($"HELIX_{parts[0].ToUpperInvariant()}__{parts[1].ToUpperInvariant()}")
				?? values.GetValueOrDefault(key, fallback);
		}
		int Number(string key, int fallback) => int.Parse(Value(key, fallback.ToString(CultureInfo.InvariantCulture)), CultureInfo.InvariantCulture);
		bool Boolean(string key, bool fallback) => Value(key, fallback ? "true" : "false").ToLowerInvariant() switch
		{
			"true" or "1" or "yes" or "on" => true,
			"false" or "0" or "no" or "off" => false,
			_ => throw new FormatException($"Invalid boolean: {key}")
		};
		var config = new EngineConfig
		{
			Ip = Value("network.ip", "0.0.0.0"), Port = Number("network.port", 5000),
			Transport = Value("network.transport", "tcp").ToLowerInvariant(),
			MaxConnections = Number("network.max_connections", 10000), Backlog = Number("network.backlog", 128),
			MaxSendQueue = Number("buffers.max_send_queue", 1024), HeaderSize = Number("framing.header_size", 4),
			BigEndian = Boolean("framing.big_endian", true), MaxMessageSize = Number("framing.max_message_size", 1048576),
			DrainTimeoutMs = Number("shutdown.drain_timeout_ms", 5000), ClusterEnabled = Boolean("cluster.enabled", false),
			WorldMaxEntities = Number("mmo.max_entities", 10000), WorldMaxPendingCommands = Number("mmo.max_pending_commands", 4096),
			WorldMaxCommandsPerTick = Number("mmo.max_commands_per_tick", 1024), WorldTickMs = Number("mmo.tick_ms", 50),
			ProtocolSerializer = Value("protocol.serializer", "protobuf").ToLowerInvariant()
		};
		if (config.Port == 0) throw new ArgumentException("network.port in file/environment must be 1..65535.");
		config.Validate();
		return config;
	}

	private static string StripComment(string value)
	{
		char quote = '\0';
		for (var i = 0; i < value.Length; i++)
		{
			var c = value[i];
			if (quote == '\0' && c is '"' or '\'') quote = c;
			else if (c == quote) quote = '\0';
			else if (quote == '\0' && c is '#' or ';') return value[..i];
		}
		return value;
	}
}
