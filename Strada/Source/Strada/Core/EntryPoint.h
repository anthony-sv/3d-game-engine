#pragma once

// Include exactly once, in the translation unit that implements Strada::CreateApplication.

#include "Strada/Core/Application.h"

int main(int argc, char** argv)
{
	return Strada::ApplicationMain(argc, argv);
}
