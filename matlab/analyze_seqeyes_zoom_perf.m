function summary = analyze_seqeyes_zoom_perf(jsonFiles, outFigure)
%ANALYZE_SEQEYES_ZOOM_PERF Plot worst-case first-zoom latency versus file size.
%
% summary = analyze_seqeyes_zoom_perf(jsonFiles)
% summary = analyze_seqeyes_zoom_perf(jsonFiles, outFigure)
%
% jsonFiles can be a string, char, or string array of JSON files produced by
% test/test_perf_zoom.py. If multiple runs exist, the plotted value is the
% mean of the runs for each .seq file.

if nargin < 1 || isempty(jsonFiles)
    jsonFiles = "results/zoom_perf.json";
end
if nargin < 2
    outFigure = "";
end

jsonFiles = string(jsonFiles);

seqFiles = strings(0, 1);
seqNames = strings(0, 1);
fileSizeMb = [];
meanZoomMs = [];
meanZoomS = [];
latencyEquivalentFps = [];
nRuns = [];
exitCode = [];
sourceJson = strings(0, 1);

for jj = 1:numel(jsonFiles)
    jsonFile = jsonFiles(jj);
    data = jsondecode(fileread(jsonFile));
    if ~isfield(data, "entries")
        warning("Skipping JSON without entries: %s", jsonFile);
        continue;
    end

    entries = data.entries;
    for ii = 1:numel(entries)
        entry = entries(ii);
        thisFile = string(entry.file);
        runs = localRunsAsDouble(entry);
        if isempty(runs)
            continue;
        end

        seqFiles(end + 1, 1) = thisFile; %#ok<AGROW>
        [~, baseName, extension] = fileparts(thisFile);
        seqNames(end + 1, 1) = string(baseName) + string(extension); %#ok<AGROW>
        fileSizeMb(end + 1, 1) = localFileSizeMb(thisFile); %#ok<AGROW>
        meanZoomMs(end + 1, 1) = mean(runs); %#ok<AGROW>
        meanZoomS(end + 1, 1) = mean(runs) / 1000; %#ok<AGROW>
        latencyEquivalentFps(end + 1, 1) = 1000 / mean(runs); %#ok<AGROW>
        nRuns(end + 1, 1) = numel(runs); %#ok<AGROW>
        exitCode(end + 1, 1) = localExitCode(entry); %#ok<AGROW>
        sourceJson(end + 1, 1) = jsonFile; %#ok<AGROW>
    end
end

raw = table();
raw.seq_file = seqFiles;
raw.seq_name = seqNames;
raw.file_size_mb = fileSizeMb;
raw.mean_zoom_ms = meanZoomMs;
raw.mean_zoom_s = meanZoomS;
raw.latency_equivalent_fps = latencyEquivalentFps;
raw.n_runs = nRuns;
raw.exit = exitCode;
raw.source_json = sourceJson;

summary = localCombineDuplicateSeqFiles(raw);
summary = sortrows(summary, "file_size_mb");

figure("Name", "SeqEyes Zoom Latency", "Color", "w");
hold on;
box on;

h = plot(summary.file_size_mb, summary.mean_zoom_s, "o", ...
    "LineStyle", "none", ...
    "MarkerSize", 7, ...
    "LineWidth", 0.9);
h.MarkerFaceColor = h.Color;
h.MarkerEdgeColor = h.Color;

xlabel(".seq file size (MB)");
ylabel("Zoom latency (s)");
title("Zoom latency");

ax = gca;
ax.FontName = "Helvetica";
ax.FontSize = 12;
ax.LineWidth = 1;
ax.TickDir = "out";
ax.Box = "off";
ax.Color = [0.985 0.985 0.975];
ax.XGrid = "on";
ax.YGrid = "on";
ax.GridColor = [0.82 0.82 0.78];
ax.GridAlpha = 0.35;

if strlength(string(outFigure)) > 0
    exportgraphics(gcf, outFigure, "Resolution", 200);
end
end

function runs = localRunsAsDouble(entry)
if isfield(entry, "runs") && ~isempty(entry.runs)
    runs = double(entry.runs(:));
    runs = runs(~isnan(runs));
elseif isfield(entry, "zoom_ms") && ~isempty(entry.zoom_ms)
    runs = double(entry.zoom_ms);
else
    runs = [];
end
end

function code = localExitCode(entry)
if isfield(entry, "exit") && ~isempty(entry.exit)
    code = double(entry.exit);
else
    code = NaN;
end
end

function sizeMb = localFileSizeMb(pathValue)
info = dir(pathValue);
if isempty(info)
    warning("File not found when computing size: %s", pathValue);
    sizeMb = NaN;
else
    sizeMb = info.bytes / 1024^2;
end
end

function summary = localCombineDuplicateSeqFiles(raw)
if isempty(raw)
    summary = raw;
    return;
end

[groups, seqFiles] = findgroups(raw.seq_file);
[~, baseNames, extensions] = arrayfun(@fileparts, seqFiles, "UniformOutput", false);

summary = table();
summary.seq_file = seqFiles;
summary.seq_name = string(baseNames) + string(extensions);
summary.file_size_mb = splitapply(@localMeanOmitNan, raw.file_size_mb, groups);
summary.mean_zoom_ms = splitapply(@localWeightedMean, raw.mean_zoom_ms, raw.n_runs, groups);
summary.mean_zoom_s = summary.mean_zoom_ms / 1000;
summary.latency_equivalent_fps = 1000 ./ summary.mean_zoom_ms;
summary.n_runs = splitapply(@sum, raw.n_runs, groups);
summary.exit = splitapply(@localMinOmitNan, raw.exit, groups);
summary.source_json = splitapply(@localJoinStrings, raw.source_json, groups);
end

function y = localWeightedMean(values, weights)
valid = ~isnan(values) & ~isnan(weights) & weights > 0;
if ~any(valid)
    y = NaN;
else
    y = sum(values(valid) .* weights(valid)) / sum(weights(valid));
end
end

function y = localMeanOmitNan(x)
x = x(~isnan(x));
if isempty(x)
    y = NaN;
else
    y = mean(x);
end
end

function y = localMinOmitNan(x)
x = x(~isnan(x));
if isempty(x)
    y = NaN;
else
    y = min(x);
end
end

function y = localJoinStrings(values)
y = strjoin(unique(string(values)), ";");
end
