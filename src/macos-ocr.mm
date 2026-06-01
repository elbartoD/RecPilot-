#include "macos-ocr.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <cstdio>
#include <string>

static std::string json_escape_cpp(const std::string &value)
{
	std::string out;
	for (unsigned char ch : value) {
		switch (ch) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (ch < 0x20) {
				char buffer[7];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
				out += buffer;
			} else {
				out.push_back(static_cast<char>(ch));
			}
		}
	}
	return out;
}

static bool normalized_clip_name_candidate(NSString *raw, std::string &out, double &score)
{
	if (!raw || raw.length == 0)
		return false;

	NSMutableString *clean = [NSMutableString string];
	for (NSUInteger i = 0; i < raw.length; ++i) {
		unichar ch = [[raw uppercaseString] characterAtIndex:i];
		if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
			const unichar normalized = ch == '-' ? static_cast<unichar>('_') : ch;
			[clean appendFormat:@"%C", normalized];
		}
	}

	if (clean.length < 5 || clean.length > 16)
		return false;

	NSArray<NSString *> *blocked = @[ @"FPS", @"SHUTTER", @"BAT", @"MEDIA", @"REC", @"RAW", @"LOOK", @"CAM",
					  @"SDI", @"HDMI", @"MON", @"WRS", @"LDS", @"TC", @"WB", @"EI", @"ND" ];
	for (NSString *word in blocked) {
		if ([clean hasPrefix:word])
			return false;
	}

	int letters = 0;
	int digits = 0;
	bool hasC = false;
	for (NSUInteger i = 0; i < clean.length; ++i) {
		unichar ch = [clean characterAtIndex:i];
		if (ch >= 'A' && ch <= 'Z') {
			letters++;
			if (ch == 'C')
				hasC = true;
		} else if (ch >= '0' && ch <= '9') {
			digits++;
		}
	}

	if (letters < 1 || digits < 4)
		return false;

	score = digits + letters * 0.7 + (hasC ? 3.0 : 0.0);
	out = std::string(clean.UTF8String);
	return true;
}

std::string recognize_text_rgba_macos(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height)
{
	if (rgba.empty() || width == 0 || height == 0)
		return {};

	@autoreleasepool {
		NSData *data = [NSData dataWithBytes:rgba.data() length:rgba.size()];
		CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
		if (!provider)
			return {};

		CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
		CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, colorSpace,
						 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, provider,
						 nullptr, false, kCGRenderingIntentDefault);
		CGColorSpaceRelease(colorSpace);
		CGDataProviderRelease(provider);

		if (!image)
			return {};

		VNRecognizeTextRequest *request = [[VNRecognizeTextRequest alloc] init];
		request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
		request.usesLanguageCorrection = YES;
		request.minimumTextHeight = 0.02f;
		if (@available(macOS 13.0, *))
			request.revision = VNRecognizeTextRequestRevision3;

		VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
		NSError *error = nil;
		const bool ok = [handler performRequests:@[ request ] error:&error];
		CGImageRelease(image);

		if (!ok || error)
			return {};

		NSMutableArray<NSString *> *lines = [NSMutableArray array];
		for (VNRecognizedTextObservation *observation in request.results) {
			VNRecognizedText *candidate = [[observation topCandidates:1] firstObject];
			if (candidate.string.length > 0)
				[lines addObject:candidate.string];
		}

		NSString *joined = [lines componentsJoinedByString:@" "];
		return joined ? std::string(joined.UTF8String) : std::string();
	}
}

std::string recognize_text_observations_json_rgba_macos(const std::vector<uint8_t> &rgba, uint32_t width,
							uint32_t height)
{
	if (rgba.empty() || width == 0 || height == 0)
		return "[]";

	@autoreleasepool {
		NSData *data = [NSData dataWithBytes:rgba.data() length:rgba.size()];
		CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
		if (!provider)
			return "[]";

		CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
		CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, colorSpace,
						 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, provider,
						 nullptr, false, kCGRenderingIntentDefault);
		CGColorSpaceRelease(colorSpace);
		CGDataProviderRelease(provider);

		if (!image)
			return "[]";

		VNRecognizeTextRequest *request = [[VNRecognizeTextRequest alloc] init];
		request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
		request.usesLanguageCorrection = YES;
		request.minimumTextHeight = 0.012f;
		if (@available(macOS 13.0, *))
			request.revision = VNRecognizeTextRequestRevision3;

		VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
		NSError *error = nil;
		const bool ok = [handler performRequests:@[ request ] error:&error];
		CGImageRelease(image);

		if (!ok || error)
			return "[]";

		std::string json = "[";
		bool first = true;
		for (VNRecognizedTextObservation *observation in request.results) {
			VNRecognizedText *candidate = [[observation topCandidates:1] firstObject];
			if (candidate.string.length == 0)
				continue;

			const CGRect box = observation.boundingBox;
			const double x = std::clamp(static_cast<double>(box.origin.x), 0.0, 1.0);
			const double y = std::clamp(1.0 - static_cast<double>(box.origin.y + box.size.height), 0.0, 1.0);
			const double w = std::clamp(static_cast<double>(box.size.width), 0.0, 1.0);
			const double h = std::clamp(static_cast<double>(box.size.height), 0.0, 1.0);

			if (!first)
				json += ",";
			first = false;
			char buffer[160];
			std::snprintf(buffer, sizeof(buffer), ",\"x\":%.6f,\"y\":%.6f,\"width\":%.6f,\"height\":%.6f", x,
				      y, w, h);
			json += "{\"text\":\"";
			json += json_escape_cpp(std::string(candidate.string.UTF8String));
			json += "\"";
			json += buffer;
			json += "}";
		}
		json += "]";
		return json;
	}
}

bool save_rgba_jpeg_macos(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height, const std::string &path)
{
	if (rgba.empty() || width == 0 || height == 0 || path.empty())
		return false;

	@autoreleasepool {
		NSData *data = [NSData dataWithBytes:rgba.data() length:rgba.size()];
		CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
		if (!provider)
			return false;

		CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
		CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, colorSpace,
						 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, provider,
						 nullptr, false, kCGRenderingIntentDefault);
		CGColorSpaceRelease(colorSpace);
		CGDataProviderRelease(provider);
		if (!image)
			return false;

		NSString *pathString = [NSString stringWithUTF8String:path.c_str()];
		NSURL *url = [NSURL fileURLWithPath:pathString];
		CFStringRef type = (__bridge CFStringRef)UTTypeJPEG.identifier;
		CGImageDestinationRef destination = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, type, 1, nullptr);
		if (!destination) {
			CGImageRelease(image);
			return false;
		}

		NSDictionary *properties = @{(__bridge NSString *)kCGImageDestinationLossyCompressionQuality : @0.88};
		CGImageDestinationAddImage(destination, image, (__bridge CFDictionaryRef)properties);
		const bool ok = CGImageDestinationFinalize(destination);
		CFRelease(destination);
		CGImageRelease(image);
		return ok;
	}
}

bool recognize_clip_name_box_rgba_macos(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height,
					double &x, double &y, double &box_width, double &box_height,
					std::string &text)
{
	if (rgba.empty() || width == 0 || height == 0)
		return false;

	@autoreleasepool {
		NSData *data = [NSData dataWithBytes:rgba.data() length:rgba.size()];
		CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
		if (!provider)
			return false;

		CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
		CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, colorSpace,
						 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, provider,
						 nullptr, false, kCGRenderingIntentDefault);
		CGColorSpaceRelease(colorSpace);
		CGDataProviderRelease(provider);

		if (!image)
			return false;

		VNRecognizeTextRequest *request = [[VNRecognizeTextRequest alloc] init];
		request.recognitionLevel = VNRequestTextRecognitionLevelFast;
		request.usesLanguageCorrection = NO;
		request.minimumTextHeight = 0.012f;
		if (@available(macOS 13.0, *))
			request.revision = VNRecognizeTextRequestRevision3;

		VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
		NSError *error = nil;
		const bool ok = [handler performRequests:@[ request ] error:&error];
		CGImageRelease(image);

		if (!ok || error)
			return false;

		double bestScore = 0.0;
		CGRect bestBox = CGRectZero;
		std::string bestText;

		for (VNRecognizedTextObservation *observation in request.results) {
			const CGRect box = observation.boundingBox;
			const double centerY = box.origin.y + box.size.height * 0.5;
			if (centerY > 0.22 && centerY < 0.78)
				continue;

			for (VNRecognizedText *candidate in [observation topCandidates:3]) {
				std::string candidateText;
				double candidateScore = 0.0;
				if (!normalized_clip_name_candidate(candidate.string, candidateText, candidateScore))
					continue;

				candidateScore += (centerY < 0.5 ? 2.0 : 1.0);
				if (candidateScore <= bestScore)
					continue;

				bestScore = candidateScore;
				bestBox = box;
				bestText = candidateText;
			}
		}

		if (bestScore <= 0.0)
			return false;

		x = std::clamp(static_cast<double>(bestBox.origin.x), 0.0, 1.0);
		y = std::clamp(1.0 - static_cast<double>(bestBox.origin.y + bestBox.size.height), 0.0, 1.0);
		box_width = std::clamp(static_cast<double>(bestBox.size.width), 0.0, 1.0);
		box_height = std::clamp(static_cast<double>(bestBox.size.height), 0.0, 1.0);
		text = bestText;
		return true;
	}
}
