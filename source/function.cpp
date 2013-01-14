// Petter Strandmark 2012-2013.

#include <iostream>
#include <stdexcept>

#include <spii/function.h>
#include <spii/spii.h>

namespace spii {

Function::Function()
{
	this->number_of_scalars = 0;
	this->term_deletion = DeleteTerms;

	this->hessian_is_enabled = true;

	this->number_of_hessian_elements = 0;

	this->evaluations_without_gradient = 0;
	this->evaluations_with_gradient    = 0;

	this->evaluate_time               = 0;
	this->evaluate_with_hessian_time  = 0;
	this->write_gradient_hessian_time = 0;
	this->copy_time                   = 0;

	#ifdef USE_OPENMP
		this->number_of_threads = omp_get_max_threads();
	#else
		this->number_of_threads = 1;
	#endif

	this->local_storage_allocated = false;
}

Function::~Function()
{
	if (this->term_deletion == DeleteTerms) {
		for (auto itr = added_terms.begin(); itr != added_terms.end(); ++itr) {
			delete *itr;
		}
	}

	// Go through all variables and destroy all change of variables objects.
	for (auto itr = variables.begin(); itr != variables.end(); ++itr) {
		if (itr->second.change_of_variables) {
			delete itr->second.change_of_variables;
		}
	}
}

void Function::add_variable_internal(double* variable,
                                     int dimension,
                                     ChangeOfVariables* change_of_variables)
{
	this->local_storage_allocated = false;

	auto itr = variables.find(variable);
	if (itr != variables.end()) {
		if (itr->second.user_dimension != dimension) {
			throw std::runtime_error("Function::add_variable: dimension mismatch.");
		}
		return;
	}
	AddedVariable& var_info = variables[variable];

	var_info.change_of_variables = change_of_variables;
	if (change_of_variables != NULL){
		if (dimension != change_of_variables->x_dimension()) {
			throw std::runtime_error("Function::add_variable: "
			                         "dimension does not match the change of variables.");
		}
		var_info.user_dimension   = change_of_variables->x_dimension();
		var_info.solver_dimension = change_of_variables->t_dimension();
	}
	else {
		var_info.user_dimension   = dimension;
		var_info.solver_dimension = dimension;
	}

	// Allocate local scratch spaces for evaluation.
	// We need as much space as the dimension of x.
	var_info.temp_space.resize(var_info.user_dimension);

	// Give this variable a global index into a global
	// state vector.
	var_info.global_index = number_of_scalars;
	number_of_scalars += var_info.solver_dimension;
}


void Function::add_term(const Term* term, const std::vector<double*>& arguments)
{
	this->local_storage_allocated = false;

	if (term->number_of_variables() != arguments.size()) {
		throw std::runtime_error("Function::add_term: incorrect number of arguments.");
	}
	for (int var = 0; var < term->number_of_variables(); ++var) {
		auto var_itr = variables.find(arguments[var]);
		if (var_itr == variables.end()) {
			throw std::runtime_error("Function::add_term: unknown variable.");
		}
		// The x-dimension of the variable must match what is expected by the term.
		if (var_itr->second.user_dimension != term->variable_dimension(var)) {
			throw std::runtime_error("Function::add_term: variable dimension does not match term.");
		}
	}

	added_terms.insert(term);

	terms.push_back(AddedTerm());
	terms.back().term = term;

	for (int var = 0; var < term->number_of_variables(); ++var) {
		// Look up this variable.
		AddedVariable& added_variable = this->variables[arguments[var]];
		terms.back().user_variables.push_back(&added_variable);
		// Stora a pointer to temporary storage for this variable.
		double* temp_space = &added_variable.temp_space[0];
		terms.back().temp_variables.push_back(temp_space);
	}

	if (this->hessian_is_enabled) {
		// Create enough space for the hessian.
		terms.back().hessian.resize(term->number_of_variables());
		for (int var0 = 0; var0 < term->number_of_variables(); ++var0) {
			terms.back().hessian[var0].resize(term->number_of_variables());
			for (int var1 = 0; var1 < term->number_of_variables(); ++var1) {
				terms.back().hessian[var0][var1].resize(term->variable_dimension(var0),
														term->variable_dimension(var1));
			}
		}
	}
}

void Function::set_number_of_threads(int num)
{
	#ifdef USE_OPENMP
		if (num <= 0) {
			throw std::runtime_error("Function::set_number_of_threads: "
			                         "invalid number of threads.");
		}
		this->local_storage_allocated = false;
		this->number_of_threads = num;
	#endif
}

void Function::allocate_local_storage() const
{
	size_t max_arity = 1;
	int max_variable_dimension = 1;
	for (auto itr = variables.begin(); itr != variables.end(); ++itr) {
		max_variable_dimension = std::max(max_variable_dimension,
		                                  itr->second.user_dimension);
	}
	for (auto itr = terms.begin(); itr != terms.end(); ++itr) {
		max_arity = std::max(max_arity, itr->user_variables.size());
	}

	this->thread_gradient_scratch.resize(this->number_of_threads);
	this->thread_gradient_storage.resize(this->number_of_threads);
	for (int t = 0; t < this->number_of_threads; ++t) {
		this->thread_gradient_storage[t].resize(number_of_scalars);
		this->thread_gradient_scratch[t].resize(max_arity);
		for (int var = 0; var < max_arity; ++var) {
			this->thread_gradient_scratch[t][var].resize(max_variable_dimension);
		}
	}

	this->local_storage_allocated = true;
}

void Function::add_term(const Term* term, double* argument0)
{
	std::vector<double*> arguments;
	arguments.push_back(argument0);
	add_term(term, arguments);
}

void Function::add_term(const Term* term, double* argument0, double* argument1)
{
	std::vector<double*> arguments;
	arguments.push_back(argument0);
	arguments.push_back(argument1);
	add_term(term, arguments);
}

void Function::print_timing_information(std::ostream& out) const
{
	out << "Function evaluations without gradient : " << evaluations_without_gradient << '\n';
	out << "Function evaluations with gradient    : " << evaluations_with_gradient << '\n';
	out << "Function evaluate time            : " << evaluate_time << '\n';
	out << "Function evaluate time (with g/H) : " << evaluate_with_hessian_time << '\n';
	out << "Function write g/H time           : " << write_gradient_hessian_time << '\n';
	out << "Function copy data time           : " << copy_time << '\n';
}

double Function::evaluate_from_local_storage() const
{
	this->evaluations_without_gradient++;
	double start_time = wall_time();

	double value = 0;
	// Go through and evaluate each term.
	// OpenMP requires a signed data type as the loop variable.
	#ifdef USE_OPENMP
		// Each thread needs to store a specific error.
		std::vector<std::string> evaluation_errors(this->number_of_threads);

		#pragma omp parallel for reduction(+ : value) num_threads(this->number_of_threads)
	#endif
	for (int i = 0; i < terms.size(); ++i) {
		#ifdef USE_OPENMP
			// The thread number calling this iteration.
			int t = omp_get_thread_num();
			// We need to catch all exceptions before leaving
			// the loop body.
			try {
		#endif

		// Evaluate the term .
		value += terms[i].term->evaluate(&terms[i].temp_variables[0]);

		#ifdef USE_OPENMP
			// We need to catch all exceptions before leaving
			// the loop body.
			}
			catch (const std::exception& error) {
				evaluation_errors[t] = error.what();
			}
			catch (...) {
				evaluation_errors[t] = "Unknown exception (not an std::exception)";
			}
		#endif
	}

	#ifdef USE_OPENMP
		// Now that we are outside the OpenMP block, we can
		// rethrow exceptions.
		for (auto itr = evaluation_errors.begin(); itr != evaluation_errors.end(); ++itr) {
			if (itr->length() > 0) {
				throw std::runtime_error(*itr);
			}
		}
	#endif

	this->evaluate_time += wall_time() - start_time;
	return value;
}

double Function::evaluate(const Eigen::VectorXd& x) const
{
	// Copy values from the global vector x to the temporary storage
	// used for evaluating the term.
	this->copy_global_to_local(x);

	return this->evaluate_from_local_storage();
}

double Function::evaluate() const
{
	// Copy the user state to the local storage
	// for evaluation.
	this->copy_user_to_local();

	return this->evaluate_from_local_storage();
}

void Function::create_sparse_hessian(Eigen::SparseMatrix<double>* H) const
{
	std::vector<Eigen::Triplet<double> > indices;
	indices.reserve(this->number_of_hessian_elements);
	this->number_of_hessian_elements = 0;

	for (auto itr = terms.begin(); itr != terms.end(); ++itr) {
		// Put the hessian into the global hessian.
		for (int var0 = 0; var0 < itr->term->number_of_variables(); ++var0) {
			size_t global_offset0 = itr->user_variables[var0]->global_index;
			for (int var1 = 0; var1 < itr->term->number_of_variables(); ++var1) {
				size_t global_offset1 = itr->user_variables[var1]->global_index;
				for (size_t i = 0; i < itr->term->variable_dimension(var0); ++i) {
					for (size_t j = 0; j < itr->term->variable_dimension(var1); ++j) {
						int global_i = static_cast<int>(i + global_offset0);
						int global_j = static_cast<int>(j + global_offset1);
						indices.push_back(Eigen::Triplet<double>(global_i,
						                                         global_j,
						                                         1.0));
						this->number_of_hessian_elements++;
					}
				}
			}
		}
	}
	H->resize(static_cast<int>(this->number_of_scalars),
	          static_cast<int>(this->number_of_scalars));
	H->setFromTriplets(indices.begin(), indices.end());
	H->makeCompressed();
}

void Function::copy_global_to_local(const Eigen::VectorXd& x) const
{
	double start_time = wall_time();

	for (auto itr = variables.begin(); itr != variables.end(); ++itr) {
		if (itr->second.change_of_variables == NULL) {
			for (int i = 0; i < itr->second.user_dimension; ++i) {
				itr->second.temp_space[i] = x[itr->second.global_index + i];
			}
		}
		else {
			itr->second.change_of_variables->t_to_x(
				&itr->second.temp_space[0],
				&x[itr->second.global_index]);
		}
	}

	this->copy_time += wall_time() - start_time;
}

void Function::copy_user_to_global(Eigen::VectorXd* x) const
{
	double start_time = wall_time();

	x->resize(this->number_of_scalars);
	for (auto itr = variables.begin(); itr != variables.end(); ++itr) {
		if (itr->second.change_of_variables == NULL) {
			for (int i = 0; i < itr->second.user_dimension; ++i) {
				(*x)[itr->second.global_index + i] = itr->first[i];
			}
		}
		else {
			itr->second.change_of_variables->x_to_t(
				&(*x)[itr->second.global_index],
				itr->first);
		}
	}

	this->copy_time += wall_time() - start_time;
}

void Function::copy_global_to_user(const Eigen::VectorXd& x) const
{
	double start_time = wall_time();

	for (auto itr = variables.begin(); itr != variables.end(); ++itr) {
		if (itr->second.change_of_variables == NULL) {
			for (int i = 0; i < itr->second.user_dimension; ++i) {
				itr->first[i] = x[itr->second.global_index + i];
			}
		}
		else {
			itr->second.change_of_variables->t_to_x(
				itr->first,
				&x[itr->second.global_index]);
		}
	}

	this->copy_time += wall_time() - start_time;
}

void Function::copy_user_to_local() const
{
	double start_time = wall_time();

	for (auto itr = variables.begin(); itr != variables.end(); ++itr) {
		for (int i = 0; i < itr->second.user_dimension; ++i) {
			itr->second.temp_space[i] = itr->first[i];
		}
	}

	this->copy_time += wall_time() - start_time;
}

double Function::evaluate(const Eigen::VectorXd& x,
                          Eigen::VectorXd* gradient) const
{
	return this->evaluate(x, gradient, reinterpret_cast<Eigen::MatrixXd*>(0));
}

double Function::evaluate(const Eigen::VectorXd& x,
                          Eigen::VectorXd* gradient,
						  Eigen::MatrixXd* hessian) const
{
	this->evaluations_with_gradient++;

	if (hessian && ! this->hessian_is_enabled) {
		throw std::runtime_error("Function::evaluate: Hessian computation is not enabled.");
	}

	if (! this->local_storage_allocated) {
		this->allocate_local_storage();
	}

	// Copy values from the global vector x to the temporary storage
	// used for evaluating the term.
	this->copy_global_to_local(x);

	double start_time = wall_time();

	// Initialize each thread's global gradient.
	for (int t = 0; t < this->number_of_threads; ++t) {
		this->thread_gradient_storage[t].setZero();
	}

	double value = 0;

	// Go through and evaluate each term.
	// OpenMP requires a signed data type as the loop variable.
	#ifdef USE_OPENMP
		// Each thread needs to store a specific error.
		std::vector<std::string> evaluation_errors(this->number_of_threads);

		#pragma omp parallel for reduction(+ : value) num_threads(this->number_of_threads)
	#endif
	for (int i = 0; i < terms.size(); ++i) {
		#ifdef USE_OPENMP
			// The thread number calling this iteration.
			int t = omp_get_thread_num();
			// We need to catch all exceptions before leaving
			// the loop body.
			try {
		#else
			int t = 0;
		#endif

		if (hessian) {
			// Evaluate the term and put its gradient and hessian
			// into local storage.
			value += terms[i].term->evaluate(&terms[i].temp_variables[0],
											 &this->thread_gradient_scratch[t],
											 &terms[i].hessian);
		}
		else {
			// Evaluate the term and put its gradient into local
			// storage.
			value += terms[i].term->evaluate(&terms[i].temp_variables[0],
											 &this->thread_gradient_scratch[t]);
		}

		// Put the gradient from the term into the thread's global gradient.
		const auto& variables = terms[i].user_variables;
		for (int var = 0; var < variables.size(); ++var) {

			if (variables[var]->change_of_variables == NULL) {
				// No change of variables, just copy the gradient.
				size_t global_offset = variables[var]->global_index;
				for (int i = 0; i < variables[var]->user_dimension; ++i) {
					this->thread_gradient_storage[t][global_offset + i] +=
						this->thread_gradient_scratch[t][var][i];
				}
			}
			else {
				// Transform the gradient from user space to solver space.
				size_t global_offset = variables[var]->global_index;
				variables[var]->change_of_variables->update_gradient(
					&this->thread_gradient_storage[t][global_offset],
					&x[global_offset],
					&this->thread_gradient_scratch[t][var][0]);
			}
		}

		#ifdef USE_OPENMP
			// We need to catch all exceptions before leaving
			// the loop body.
			}
			catch (const std::exception& error) {
				evaluation_errors[t] = error.what();
			}
			catch (...) {
				evaluation_errors[t] = "Unknown exception (not an std::exception)";
			}
		#endif
	}

	#ifdef USE_OPENMP
		// Now that we are outside the OpenMP block, we can
		// rethrow exceptions.
		for (auto itr = evaluation_errors.begin(); itr != evaluation_errors.end(); ++itr) {
			if (itr->length() > 0) {
				throw std::runtime_error(*itr);
			}
		}
	#endif

	this->evaluate_with_hessian_time += wall_time() - start_time;
	start_time = wall_time();

	// Create the global gradient.
	if (gradient->size() != this->number_of_scalars) {
		gradient->resize(this->number_of_scalars);
	}
	gradient->setZero();
	// Sum the gradients from all different terms.
	for (int t = 0; t < this->number_of_threads; ++t) {
		(*gradient) += this->thread_gradient_storage[t];
	}

	if (hessian) {
		// Create the global (dense) hessian.
		hessian->resize( static_cast<int>(this->number_of_scalars),
						 static_cast<int>(this->number_of_scalars));
		hessian->setZero();

		// Go through and evaluate each term.
		for (auto itr = terms.begin(); itr != terms.end(); ++itr) {
			// Put the hessian into the global hessian.
			for (int var0 = 0; var0 < itr->term->number_of_variables(); ++var0) {

				if (itr->user_variables[var0]->change_of_variables != NULL) {
					throw std::runtime_error("Change of variables not supported for Hessians");
				}

				size_t global_offset0 = itr->user_variables[var0]->global_index;
				for (int var1 = 0; var1 < itr->term->number_of_variables(); ++var1) {
					size_t global_offset1 = itr->user_variables[var1]->global_index;
					const Eigen::MatrixXd& part_hessian = itr->hessian[var0][var1];
					for (int i = 0; i < itr->term->variable_dimension(var0); ++i) {
						for (int j = 0; j < itr->term->variable_dimension(var1); ++j) {
							hessian->coeffRef(i + global_offset0, j + global_offset1) +=
								part_hessian(i, j);
						}
					}
				}
			}
		}
	}

	this->write_gradient_hessian_time += wall_time() - start_time;
	return value;
}

double Function::evaluate(const Eigen::VectorXd& x,
                          Eigen::VectorXd* gradient,
						  Eigen::SparseMatrix<double>* hessian) const
{
	this->evaluations_with_gradient++;

	if (! hessian) {
		throw std::runtime_error("Function::evaluate: hessian can not be null.");
	}

	if (! this->hessian_is_enabled) {
		throw std::runtime_error("Function::evaluate: Hessian computation is not enabled.");
	}

	if (! this->local_storage_allocated) {
		this->allocate_local_storage();
	}

	// Copy values from the global vector x to the temporary storage
	// used for evaluating the term.
	this->copy_global_to_local(x);

	double start_time = wall_time();

	std::vector<Eigen::Triplet<double> > indices;
	indices.reserve(this->number_of_hessian_elements);
	this->number_of_hessian_elements = 0;

	this->write_gradient_hessian_time += wall_time() - start_time;
	start_time = wall_time();

	// Initialize each thread's global gradient.
	for (int t = 0; t < this->number_of_threads; ++t) {
		this->thread_gradient_storage[t].setZero();
	}

	double value = 0;
	// Go through and evaluate each term.
	// OpenMP requires a signed data type as the loop variable.
	#ifdef USE_OPENMP
		// Each thread needs to store a specific error.
		std::vector<std::string> evaluation_errors(this->number_of_threads);

		#pragma omp parallel for reduction(+ : value) num_threads(this->number_of_threads)
	#endif
	for (int i = 0; i < terms.size(); ++i) {
		#ifdef USE_OPENMP
			// The thread number calling this iteration.
			int t = omp_get_thread_num();
			// We need to catch all exceptions before leaving
			// the loop body.
			try {
		#else
			int t = 0;
		#endif

		// Evaluate the term and put its gradient and hessian
		// into local storage.
		value += terms[i].term->evaluate(&terms[i].temp_variables[0],
		                                 &this->thread_gradient_scratch[t],
		                                 &terms[i].hessian);

		// Put the gradient from the term into the thread's global gradient.
		const auto& variables = terms[i].user_variables;
		for (int var = 0; var < variables.size(); ++var) {

			if (variables[var]->change_of_variables != NULL) {
				throw std::runtime_error("Change of variables not supported for sparse Hessian");
			}

			size_t global_offset = variables[var]->global_index;
			for (int i = 0; i < variables[var]->user_dimension; ++i) {
				this->thread_gradient_storage[t][global_offset + i] +=
					this->thread_gradient_scratch[t][var][i];
			}
		}

		#ifdef USE_OPENMP
			// We need to catch all exceptions before leaving
			// the loop body.
			}
			catch (const std::exception& error) {
				evaluation_errors[t] = error.what();
			}
			catch (...) {
				evaluation_errors[t] = "Unknown exception (not an std::exception)";
			}
		#endif
	}

	#ifdef USE_OPENMP
		// Now that we are outside the OpenMP block, we can
		// rethrow exceptions.
		for (auto itr = evaluation_errors.begin(); itr != evaluation_errors.end(); ++itr) {
			if (itr->length() > 0) {
				throw std::runtime_error(*itr);
			}
		}
	#endif

	this->evaluate_with_hessian_time += wall_time() - start_time;
	start_time = wall_time();

	// Create the global gradient.
	if (gradient->size() != this->number_of_scalars) {
		gradient->resize(this->number_of_scalars);
	}
	gradient->setZero();
	// Sum the gradients from all different terms.
	for (int t = 0; t < this->number_of_threads; ++t) {
		(*gradient) += this->thread_gradient_storage[t];
	}

	// Collect the gradients and hessians from each term.
	for (auto itr = terms.begin(); itr != terms.end(); ++itr) {
		// Put the hessian into the global hessian.
		for (int var0 = 0; var0 < itr->term->number_of_variables(); ++var0) {
			size_t global_offset0 = itr->user_variables[var0]->global_index;
			for (int var1 = 0; var1 < itr->term->number_of_variables(); ++var1) {
				size_t global_offset1 = itr->user_variables[var1]->global_index;
				const Eigen::MatrixXd& part_hessian = itr->hessian[var0][var1];
				for (int i = 0; i < itr->term->variable_dimension(var0); ++i) {
					for (int j = 0; j < itr->term->variable_dimension(var1); ++j) {
						//std::cerr << "var=(" << var0 << ',' << var1 << ") ";
						//std::cerr << "ij=(" << i << ',' << j << ") ";
						//std::cerr << "writing to (" << i + global_offset0 << ',' << j + global_offset1 << ")\n";
						//hessian->coeffRef(i + global_offset0, j + global_offset1) +=
						//	part_hessian(i, j);
						int global_i = static_cast<int>(i + global_offset0);
						int global_j = static_cast<int>(j + global_offset1);
						indices.push_back(Eigen::Triplet<double>(global_i,
						                                         global_j,
						                                         part_hessian(i, j)));
						this->number_of_hessian_elements++;
					}
				}
			}
		}
	}

	hessian->setFromTriplets(indices.begin(), indices.end());
	//hessian->makeCompressed();

	this->write_gradient_hessian_time += wall_time() - start_time;

	return value;
}

}  // namespace spii