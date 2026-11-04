using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using BenchmarkDotNet.Attributes;
using BenchmarkDotNet.Configs;
using BenchmarkDotNet.Jobs;
using BenchmarkDotNet.Running;
using Perfolizer.Horology;

namespace Helix.WireBenchmarks;

public static class Program
{
	public static void Main(string[] args)
	{
		if (args.Contains("--smoke"))
		{
			foreach (var name in WireCase.Names) foreach (var format in WireCase.Formats) { var item = new WireCase(name, format); GC.KeepAlive(item.DecodeValidated()); }
			Console.WriteLine("PASS: 27 production-codec framed/validated round-trips"); return;
		}
		var outputAt = Array.IndexOf(args, "--out");
		var output = outputAt >= 0 ? args[outputAt + 1] : "out/wire-managed.json";
		var artifacts = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(output))!, "wire-bdn");
		Directory.CreateDirectory(artifacts);
		var job = Job.ShortRun.WithId("WireValidated").WithLaunchCount(1).WithWarmupCount(3).WithIterationCount(9)
			.WithIterationTime(TimeInterval.FromMilliseconds(100)).WithEnvironmentVariable("DOTNET_TieredCompilation", "0");
		var config = ManualConfig.Create(DefaultConfig.Instance).AddJob(job).WithArtifactsPath(artifacts);
		var summaries = BenchmarkRunner.Run<WireBenchmarks>(config);
		var rows = new List<object>();
		foreach (var report in summaries.Reports)
		{
			if (report.ResultStatistics is null || !report.Success) throw new InvalidOperationException("A benchmark failed; no partial result export.");
			var name = (string)report.BenchmarkCase.Parameters.Items.Single(p => p.Name == nameof(WireBenchmarks.Message)).Value;
			var format = (string)report.BenchmarkCase.Parameters.Items.Single(p => p.Name == nameof(WireBenchmarks.Format)).Value;
			var wire = new WireCase(name, format);
			var allocated = report.Metrics.Single(pair => pair.Value.Descriptor.Id == "Allocated Memory").Value.Value;
			rows.Add(new { Message = name, Format = format, Operation = report.BenchmarkCase.Descriptor.WorkloadMethod.Name,
				WireBytes = wire.Input.Length, TcpHeaderBytes = 4, EnvelopeBytes = wire.EnvelopeBytes,
				MeanNs = report.ResultStatistics.Mean, MedianNs = report.ResultStatistics.Median,
				ErrorNs = report.ResultStatistics.ConfidenceInterval.Margin, StdDevNs = report.ResultStatistics.StandardDeviation,
				AllocatedBytes = allocated,
				SamplesNs = report.AllMeasurements.Where(m => m.IterationStage == BenchmarkDotNet.Engines.IterationStage.Result && m.IterationMode == BenchmarkDotNet.Engines.IterationMode.Workload).Select(m => m.Nanoseconds / m.Operations).ToArray() });
		}
		File.WriteAllText(output, JsonSerializer.Serialize(new { Contract = "production codec + 4-byte TCP frame + complete DTO + semantic validation; socket I/O excluded",
			Framework = RuntimeInformation.FrameworkDescription, OS = RuntimeInformation.OSDescription, Architecture = RuntimeInformation.ProcessArchitecture.ToString(),
			ProcessorCount = Environment.ProcessorCount, Utc = DateTimeOffset.UtcNow, WarmupIterations = 3, MeasurementIterations = 9,
			IterationTimeMs = 100, Launches = 1, TieredCompilation = "0", Mode = "BenchmarkDotNet 0.15.8 isolated processes, MemoryDiagnoser", Rows = rows }, new JsonSerializerOptions { WriteIndented = true }));
		Console.WriteLine($"Raw production wire result: {output}");
	}
}

[MemoryDiagnoser]
public class WireBenchmarks
{
	[Params("Move", "Attack", "Look", "Welcome", "Reply", "State0", "State16", "State256", "State4096")]
	public string Message { get; set; } = "";
	[Params("protobuf", "flatbuffers", "memorypack")]
	public string Format { get; set; } = "";
	private WireCase _case = null!;
	[GlobalSetup] public void Setup() => _case = new(Message, Format);
	[Benchmark] public byte[] EncodeFramed() => _case.EncodeFramed();
	[Benchmark] public object DecodeValidated() => _case.DecodeValidated();
}
